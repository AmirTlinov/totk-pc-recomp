// Sampling profiler (TOTK_PROFILE=<path>) and per-frame timing (TOTK_TRACE=perf). tools/prof_report.py reads the profile.
// A host thread wakes every 1 ms and, for every other thread, suspends it, reads Rip and resumes it. The sampler never
// allocates or takes a lock between SuspendThread and ResumeThread and never runs guest code. The profile is text:
//   totk-prof 1 / base <hex> / period_us / samples N / thread <tid> <samples> <in-exe samples> / module <base> <size> <path> /
//   rip <hex> <count> (the top PROF_TOP Rips) / stack <count> <rip> <caller> ... (TOTK_PROFILE_RENDER or
//   TOTK_PROFILE_CHAINS only, the top PROF_STACKS call chains)
// With TOTK_PROFILE_RENDER (the render thread) or TOTK_PROFILE_CHAINS=1 (every thread, chains of all threads summed: for
// short frame ranges, as each sample copies a stack) the sampler also copies up to STK_COPY bytes of the suspended
// thread's stack and, after
// ResumeThread, unwinds the copy with the .pdata tables (RtlVirtualUnwind), so it takes no lock while the thread is
// suspended. The unwind stops at the first frame outside the copy; deeper callers are cut.
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "gfx.h"
#include "host.h"
#include "perfmon.h"

#define PROF_SLOTS (1u << 16)   // Rip hash table, open addressing; a full table drops new Rips
#define PROF_TOP 4000           // more than the 200 asked for: the report sums per function
#define PROF_THREADS 512
#define PROF_DEPTH 24             // frames kept per call chain, the sampled Rip first
#define STK_SLOTS (1u << 15)      // call-chain hash table, open addressing; a full table drops new chains
#define PROF_STACKS 6000
#define STK_COPY (64u << 10)      // stack bytes copied per sample
#define STK_SLACK (64u << 10)     // zeros after the copy: a frame reaching past it reads zeros, and a zero Rip ends the chain

typedef struct { uint64_t rip, n; } RipCount;
typedef struct { DWORD tid; HANDLE h; uint64_t n, in_exe; } ThreadSlot;

static RipCount table[PROF_SLOTS];
typedef struct { uint64_t hash, n; uint32_t depth; uint64_t rip[PROF_DEPTH]; } StackCount;
static StackCount stacks[STK_SLOTS];
static uint64_t stacks_dropped;
static uint8_t stk_buf[STK_COPY + STK_SLACK] __attribute__((aligned(16)));   // the slack is never written
static ThreadSlot threads[PROF_THREADS];
static int n_threads;
static uint64_t total, dropped;
static const char *out_path;
static HANDLE sampler;
static volatile LONG stop, written;
static uintptr_t exe_lo, exe_hi;
static volatile DWORD render_tid;   // the thread that presents (perf_present)
static int only_render;             // TOTK_PROFILE_RENDER=1: sample the render thread only
static volatile DWORD main_tid;     // the guest main thread (kern_main_thread)
static int only_main;               // TOTK_PROFILE_MAIN=1: sample the guest main thread only
static int chains;                  // call chains are kept: TOTK_PROFILE_RENDER or TOTK_PROFILE_CHAINS
static volatile uint64_t cur_frame; // last presented frame
static uint64_t frame_lo, frame_hi; // TOTK_PROFILE_FRAMES=<lo>-<hi>: sample only while presents lo..hi-1 are pending

static void count_rip(uint64_t rip) {
    uint32_t i = (uint32_t)((rip * 0x9E3779B97F4A7C15ull) >> 48);
    for (uint32_t k = 0; k < 64; k++, i = (i + 1) & (PROF_SLOTS - 1)) {
        if (table[i].rip == rip || !table[i].rip) {
            table[i].rip = rip;
            table[i].n++;
            return;
        }
    }
    dropped++;
}

static void count_stack(const uint64_t *rip, uint32_t depth) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t d = 0; d < depth; d++) h = (h ^ rip[d]) * 0x100000001b3ull;
    h |= 1;   // 0 marks a free slot
    uint32_t i = (uint32_t)(h >> 49);
    for (uint32_t k = 0; k < 64; k++, i = (i + 1) & (STK_SLOTS - 1)) {
        StackCount *s = &stacks[i];
        if (!s->hash) {
            s->hash = h, s->depth = depth;
            memcpy(s->rip, rip, depth * sizeof *rip);
        } else if (s->hash != h || s->depth != depth || memcmp(s->rip, rip, depth * sizeof *rip)) {
            continue;
        }
        s->n++;
        return;
    }
    stacks_dropped++;
}

// Unwinds the stack copied to stk_buf from real address lo (n bytes). Registers that point into the copied range are
// moved into the copy, so frame-pointer frames read the copy too. Returns the number of frames.
static uint32_t unwind_copy(CONTEXT *c, uintptr_t lo, size_t n, uint64_t *rip) {
    uintptr_t buf = (uintptr_t)stk_buf, end = buf + n;
    DWORD64 *regs[] = {&c->Rbx, &c->Rbp, &c->Rsi, &c->Rdi, &c->R12, &c->R13, &c->R14, &c->R15};
    for (unsigned i = 0; i < sizeof regs / sizeof *regs; i++)
        if (*regs[i] >= lo && *regs[i] < lo + n) *regs[i] = *regs[i] - lo + buf;
    c->Rsp = buf;
    uint32_t d = 0;
    while (d < PROF_DEPTH && c->Rip && c->Rsp >= buf && c->Rsp + 8 <= end) {
        rip[d++] = c->Rip;
        DWORD64 base;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c->Rip, &base, NULL);
        if (f) {
            void *hd;
            DWORD64 est;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, f, c, &hd, &est, NULL);
        } else {   // a leaf function: the return address is on top
            c->Rip = *(DWORD64 *)c->Rsp;
            c->Rsp += 8;
        }
    }
    return d;
}

// Re-lists the process's threads: keeps the handle of a thread seen before, opens new ones, closes the gone ones.
static void refresh_threads(DWORD self) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    static ThreadSlot next[PROF_THREADS];
    int n = 0;
    THREADENTRY32 te = {.dwSize = sizeof te};
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || n == PROF_THREADS) continue;
        ThreadSlot *old = NULL;
        for (int i = 0; i < n_threads; i++)
            if (threads[i].tid == te.th32ThreadID) old = &threads[i];
        if (old) {
            next[n++] = *old;
            old->h = NULL;
        } else {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (h) next[n++] = (ThreadSlot){.tid = te.th32ThreadID, .h = h};
        }
    }
    CloseHandle(snap);
    for (int i = 0; i < n_threads; i++)
        if (threads[i].h) CloseHandle(threads[i].h);   // exited; its counts are lost with the slot
    memcpy(threads, next, sizeof(ThreadSlot) * n);
    n_threads = n;
}

static DWORD WINAPI sampler_main(void *arg) {
    (void)arg;
    DWORD self = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    static CONTEXT ctx __attribute__((aligned(16)));
    for (unsigned tick = 0; !stop; tick++) {
        if (timer) {
            LARGE_INTEGER due = {.QuadPart = -10000};   // 1 ms
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, 10);
        } else {
            Sleep(1);
        }
        if (tick % 64 == 0) refresh_threads(self);
        if (frame_hi && (cur_frame < frame_lo || cur_frame >= frame_hi)) continue;
        for (int i = 0; i < n_threads; i++) {
            ThreadSlot *t = &threads[i];
            if ((only_render && t->tid != render_tid) || (only_main && t->tid != main_tid)) continue;
            ctx.ContextFlags = chains ? CONTEXT_CONTROL | CONTEXT_INTEGER : CONTEXT_CONTROL;
            if (SuspendThread(t->h) == (DWORD)-1) continue;
            BOOL ok = GetThreadContext(t->h, &ctx);
            size_t n = 0;
            MEMORY_BASIC_INFORMATION mb;
            if (ok && chains && VirtualQuery((void *)ctx.Rsp, &mb, sizeof mb) && mb.State == MEM_COMMIT) {
                n = (uintptr_t)mb.BaseAddress + mb.RegionSize - ctx.Rsp;   // the committed stack above Rsp
                if (n > STK_COPY) n = STK_COPY;
                memcpy(stk_buf, (void *)ctx.Rsp, n);
            }
            ResumeThread(t->h);
            if (!ok) continue;
            count_rip(ctx.Rip);
            if (n) {
                uint64_t chain[PROF_DEPTH];
                uint32_t d = unwind_copy(&ctx, ctx.Rsp, n, chain);
                if (d) count_stack(chain, d);
            }
            total++;
            t->n++;
            if (ctx.Rip >= exe_lo && ctx.Rip < exe_hi) t->in_exe++;
        }
    }
    return 0;
}

static int cmp_desc(const void *a, const void *b) {
    uint64_t x = ((const RipCount *)a)->n, y = ((const RipCount *)b)->n;
    return (x < y) - (x > y);
}

static int cmp_stack_desc(const void *a, const void *b) {
    uint64_t x = ((const StackCount *)a)->n, y = ((const StackCount *)b)->n;
    return (x < y) - (x > y);
}

void prof_write(void) {
    perf_summary_flush();
    if (!out_path || InterlockedExchange(&written, 1)) return;
    InterlockedExchange(&stop, 1);
    if (WaitForSingleObject(sampler, 2000) != WAIT_OBJECT_0) return;   // sampler stuck: no half-written counts
    FILE *f = fopen(out_path, "w");
    if (!f) {
        rt_log("prof: cannot write %s", out_path);
        return;
    }
    fprintf(f, "totk-prof 1\nbase %llx\nperiod_us 1000\nsamples %llu\ndropped %llu\n", (unsigned long long)(uintptr_t)GetModuleHandleW(NULL),
            (unsigned long long)total, (unsigned long long)dropped);
    for (int i = 0; i < n_threads; i++)
        fprintf(f, "thread %lu %llu %llu\n", threads[i].tid, (unsigned long long)threads[i].n, (unsigned long long)threads[i].in_exe);
    HMODULE mods[1024];
    DWORD need = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) {
        for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; i++) {
            MODULEINFO mi;
            char path[MAX_PATH];
            if (GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi) && GetModuleFileNameA(mods[i], path, sizeof path))
                fprintf(f, "module %llx %lx %s\n", (unsigned long long)(uintptr_t)mi.lpBaseOfDll, mi.SizeOfImage, path);
        }
    }
    int cnt = 0;
    for (uint32_t i = 0; i < PROF_SLOTS; i++)
        if (table[i].rip) table[cnt++] = table[i];
    qsort(table, cnt, sizeof *table, cmp_desc);
    for (int i = 0; i < cnt && i < PROF_TOP; i++) fprintf(f, "rip %llx %llu\n", (unsigned long long)table[i].rip, (unsigned long long)table[i].n);
    int ns = 0;
    for (uint32_t i = 0; i < STK_SLOTS; i++)
        if (stacks[i].hash) stacks[ns++] = stacks[i];
    qsort(stacks, ns, sizeof *stacks, cmp_stack_desc);
    for (int i = 0; i < ns && i < PROF_STACKS; i++) {
        fprintf(f, "stack %llu", (unsigned long long)stacks[i].n);
        for (uint32_t d = 0; d < stacks[i].depth; d++) fprintf(f, " %llx", (unsigned long long)stacks[i].rip[d]);
        fputc('\n', f);
    }
    if (stacks_dropped) fprintf(f, "stacks_dropped %llu\n", (unsigned long long)stacks_dropped);
    fclose(f);
    rt_log("prof: %llu samples, %d Rips -> %s", (unsigned long long)total, cnt, out_path);
}

// TOTK_TRACE=perf, TOTK_FLIGHT=1 or the full performance overlay: event counts are taken. Wait times are always taken
// (frame-time summary). perf_base: the part fixed at start.
static int perf_on, perf_base;
static int overlay;   // performance overlay mode (perf_set_overlay): 0 off, 1 fps, 2 full

void perf_set_overlay(int mode) {
    __atomic_store_n(&overlay, mode, __ATOMIC_RELAXED);
    __atomic_store_n(&perf_on, perf_base || mode == 2, __ATOMIC_RELAXED);
}

int perf_gpu_timing(void) { return (rt_trace & RT_TRACE_PERF) || __atomic_load_n(&overlay, __ATOMIC_RELAXED) == 2; }

static volatile LONG64 gpu_busy_total;

void perf_gpu_busy(uint64_t ns) { InterlockedAdd64(&gpu_busy_total, (LONG64)ns); }

uint64_t perf_gpu_busy_total(void) { return (uint64_t)__atomic_load_n(&gpu_busy_total, __ATOMIC_RELAXED); }

void prof_start(void) {
    flight_init();
    atexit(perf_summary_flush);   // boot.c's _exit path goes through prof_write instead
    perf_base = (rt_trace & RT_TRACE_PERF) || flight_on;
    perf_on = perf_base || overlay == 2;
    out_path = cfg_get("TOTK_PROFILE");
    if (!out_path || !*out_path) {
        out_path = NULL;
        return;
    }
    only_render = cfg_get("TOTK_PROFILE_RENDER") != NULL;
    only_main = !only_render && cfg_get("TOTK_PROFILE_MAIN") != NULL;
    const char *ch = cfg_get("TOTK_PROFILE_CHAINS");
    chains = only_render || only_main || (ch && *ch && strcmp(ch, "0"));
    const char *fr = cfg_get("TOTK_PROFILE_FRAMES");
    if (fr && sscanf(fr, "%llu-%llu", (unsigned long long *)&frame_lo, (unsigned long long *)&frame_hi) != 2) frame_hi = 0;
    MODULEINFO mi;
    if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(NULL), &mi, sizeof mi)) {
        exe_lo = (uintptr_t)mi.lpBaseOfDll;
        exe_hi = exe_lo + mi.SizeOfImage;
    }
    sampler = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
    if (!sampler) {
        rt_log("prof: cannot start the sampler (error %lu)", GetLastError());
        out_path = NULL;
        return;
    }
    atexit(prof_write);   // a --frames run ends in _exit: boot.c calls prof_write itself first
}

// ---- TOTK_TRACE=perf: frame interval and the time the render thread spends waiting, logged every 60 presents. The
// flight recorder (TOTK_FLIGHT=1, flight.c) takes the same numbers per frame.
static uint64_t wait_ns[PERF_N], frame_ns[PERF_N], worst_ns[PERF_N];   // window totals, this frame, the slowest frame

uint64_t perf_begin(void) { return kern_now_ns(); }

static volatile DWORD replay_tid;   // asynchronous replay (vk_backend.c): its waits hold up the frame too

void perf_replay_thread(void) { replay_tid = GetCurrentThreadId(); }
void perf_main_thread(void) { main_tid = GetCurrentThreadId(); }

void perf_end(int kind, uint64_t t0) {
    if (!t0) return;
    DWORD tid = GetCurrentThreadId();
    if (tid != render_tid && tid != replay_tid) return;
    uint64_t d = kern_now_ns() - t0;
    if (tid == render_tid && !replay_tid) {   // one thread adds: no atomics on the synchronous path
        wait_ns[kind] += d;
        frame_ns[kind] += d;
        return;
    }
    __atomic_fetch_add(&wait_ns[kind], d, __ATOMIC_RELAXED);
    __atomic_fetch_add(&frame_ns[kind], d, __ATOMIC_RELAXED);
}

static volatile LONG64 other_wait_ns;     // perf_gpu_wait on threads other than the render thread, over the window
static volatile LONG64 render_wait_ns;    // the same on the render thread
void (*perf_gpu_stats)(PerfGpu *out);

void perf_gpu_wait(uint64_t t0) {
    if (!t0 || !__atomic_load_n(&perf_on, __ATOMIC_RELAXED)) return;
    LONG64 d = (LONG64)(kern_now_ns() - t0);
    InterlockedAdd64(GetCurrentThreadId() == render_tid ? &render_wait_ns : &other_wait_ns, d);
}

static volatile LONG64 replay_ns;         // perf_replay: summed over the window, any thread
static volatile LONG64 frame_replay_ns;   // the same since the previous present (frame-time summary)

void perf_replay(uint64_t t0) {
    if (!t0) return;
    LONG64 d = (LONG64)(kern_now_ns() - t0);
    InterlockedAdd64(&frame_replay_ns, d);
    if (__atomic_load_n(&perf_on, __ATOMIC_RELAXED)) InterlockedAdd64(&replay_ns, d);
}

static volatile LONG64 counts[PERF_CNT_N];   // running totals: the perf log and the flight recorder take differences

void perf_count(int kind) {
    if (__atomic_load_n(&perf_on, __ATOMIC_RELAXED)) InterlockedIncrement64(&counts[kind]);
}

// ---- Frame-time summary, always on: every 60 s of play and at exit one line of frame count, mean fps, 1% low and the
// frames over 25/50/100 ms, then the worst frames of the period with the render thread's waits in them. Per frame: a
// histogram increment, the compare against the worst list, a memset of the wait array. No allocation.
#define FT_BINS 1024              // 0.25 ms bins up to 256 ms; slower frames land in the last bin
#define FT_WORST 5
#define FT_PERIOD_NS 60000000000ull
typedef struct { uint64_t frame, ns, wait[PERF_N], replay; } FtWorst;
static struct {
    uint64_t t0, frames, sum, over25, over50, over100;
    uint32_t hist[FT_BINS];
    FtWorst worst[FT_WORST];   // slowest first
    int nworst;
} ft;
static uint64_t summary_t0;   // start of the current period (0: not started)

static void ft_report(uint64_t now, const char *tag) {
    if (ft.frames && ft.sum) {
        uint64_t cut = (ft.frames + 99) / 100, acc = 0, low = 0;   // the slowest 1% of frames
        for (int b = FT_BINS - 1; b >= 0; b--) {
            acc += ft.hist[b];
            if (acc >= cut) { low = (uint64_t)(b + 1) * 250000ull; break; }   // upper edge of the bin
        }
        rt_log("frametime: %s: %llu frames in %.1f s, mean %.1f fps, 1%% low %.1f fps, over 25 ms %llu, 50 ms %llu, 100 ms %llu",
               tag, (unsigned long long)ft.frames, (now - ft.t0) / 1e9, ft.frames * 1e9 / ft.sum, low ? 1e9 / low : 0.0,
               (unsigned long long)ft.over25, (unsigned long long)ft.over50, (unsigned long long)ft.over100);
        for (int i = 0; i < ft.nworst; i++) {
            const FtWorst *w = &ft.worst[i];
            rt_log("frametime:   worst frame %llu: %.1f ms; shader translation %.1f, pipeline build %.1f, texture upload %.1f (astc %.1f), replay %.1f, fence/present wait %.1f, pacing %.1f",
                   (unsigned long long)w->frame, w->ns / 1e6, w->wait[PERF_XLATE] / 1e6, w->wait[PERF_PIPE] / 1e6,
                   w->wait[PERF_TEX] / 1e6, w->wait[PERF_ASTC] / 1e6, w->replay / 1e6,
                   (w->wait[PERF_FENCE] + w->wait[PERF_DRAIN]) / 1e6, w->wait[PERF_PACE] / 1e6);
        }
    }
    memset(&ft, 0, sizeof ft);
    ft.t0 = now;
}

__attribute__((weak)) void gfx_exit_log(void) {}

void perf_summary_flush(void) {   // at exit (prof_write)
    if (summary_t0) {
        ft_report(kern_now_ns(), "exit");
        gfx_exit_log();
    }
    summary_t0 = 0;
}

// Returns the replay time since the previous present.
static uint64_t ft_frame(uint64_t frame, uint64_t now, uint64_t d) {
    LONG64 rep = InterlockedExchange64(&frame_replay_ns, 0);
    if (!summary_t0) {   // the first present starts the period; there is no interval yet
        summary_t0 = now;
        ft.t0 = now;
        return (uint64_t)rep;
    }
    ft.frames++;
    ft.sum += d;
    uint64_t b = d / 250000ull;
    ft.hist[b < FT_BINS ? b : FT_BINS - 1]++;
    if (d > 25000000ull) ft.over25++;
    if (d > 50000000ull) ft.over50++;
    if (d > 100000000ull) ft.over100++;
    if (d > 25000000ull && (ft.nworst < FT_WORST || d > ft.worst[FT_WORST - 1].ns)) {
        int i = ft.nworst < FT_WORST ? ft.nworst++ : FT_WORST - 1;
        while (i > 0 && ft.worst[i - 1].ns < d) ft.worst[i] = ft.worst[i - 1], i--;
        ft.worst[i].frame = frame;
        ft.worst[i].ns = d;
        memcpy(ft.worst[i].wait, frame_ns, sizeof frame_ns);
        ft.worst[i].replay = (uint64_t)rep;
    }
    if (now - summary_t0 >= FT_PERIOD_NS) {
        ft_report(now, "60 s");
        summary_t0 = now;
    }
    return (uint64_t)rep;
}

// The second line of the 60-frame window: GPU busy time (timestamps), the time threads waited for GPU fences, VRAM and
// process memory, and which side holds the frame back. pace: the frame waited for pacing (vsync, the frame limiter) at
// least 15% of the window; gpu: the GPU was busy for 80% of the frame interval or more (without timestamps: threads
// waited for fences for half the interval or more); cpu otherwise.
static void perf_gpu_line(uint64_t frame, double interval_ms, double pace_share, uint64_t n) {
    PerfGpu g;
    memset(&g, 0, sizeof g);
    if (perf_gpu_stats) perf_gpu_stats(&g);
    double rw = InterlockedExchange64(&render_wait_ns, 0) / 1e6 / n, ow = InterlockedExchange64(&other_wait_ns, 0) / 1e6 / n;
    double gpu_ms = g.have_gpu ? g.gpu_ns / 1e6 / n : 0;
    char gs[96], vs[96];
    if (g.have_gpu) snprintf(gs, sizeof gs, "gpu avg %.2f ms (%llu parts timed, %llu not)", gpu_ms,
                             (unsigned long long)g.gpu_parts, (unsigned long long)g.gpu_missed);
    else snprintf(gs, sizeof gs, "gpu avg n/a");
    if (g.have_vram) snprintf(vs, sizeof vs, "vram %.0f of %.0f MiB%s", g.vram_used / 1048576.0, g.vram_budget / 1048576.0,
                              g.vram_exact ? "" : " (own allocations of the heap size)");
    else snprintf(vs, sizeof vs, "vram n/a");
    PROCESS_MEMORY_COUNTERS_EX pm = {.cb = sizeof pm};
    double ws = 0, priv = 0;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm))
        ws = pm.WorkingSetSize / 1048576.0, priv = pm.PrivateUsage / 1048576.0;
    const char *bound = pace_share >= 0.15 ? "pace"
                      : g.have_gpu ? (gpu_ms >= 0.8 * interval_ms ? "gpu" : "cpu")
                      : (rw + ow >= 0.5 * interval_ms ? "gpu" : "cpu");
    rt_log("perf: frame %llu: %s; fence wait per frame: render thread %.2f ms, other threads %.2f ms; %s; ram working set "
           "%.0f MiB, private %.0f MiB; bound: %s",
           (unsigned long long)frame, gs, rw, ow, vs, ws, priv, bound);
}

void perf_present(uint64_t frame) {
    static uint64_t last, sum, max, n, win_start;
    static LONG64 win_prev[PERF_CNT_N];
    render_tid = GetCurrentThreadId();
    cur_frame = frame;
    if (rt_trace & RT_TRACE_SCHED) kern_sched_report(frame);
    uint64_t now = kern_now_ns(), d = last ? now - last : 0;
    uint64_t rep = ft_frame(frame, now, d);   // d is 0 on the first present: it only starts the period
    if (__atomic_load_n(&overlay, __ATOMIC_RELAXED))   // the overlay's only cost while it is off: this branch
        perfmon_frame(now, d, frame_ns, rep, __atomic_load_n(&counts[PERF_CNT_TEX_UP], __ATOMIC_RELAXED));
    kern_core_diag_frame(frame, d);
    if (flight_on) flight_frame(frame, now, d, frame_ns, counts);
    if (!(rt_trace & RT_TRACE_PERF)) {
        memset(frame_ns, 0, sizeof frame_ns);
        last = now;
        return;
    }
    static uint64_t slow_ns;   // TOTK_PERF_SLOW_MS (default 50): frames at least this long get a line of their own
    if (!slow_ns) {
        const char *e = cfg_get("TOTK_PERF_SLOW_MS");
        slow_ns = (uint64_t)(e && atoi(e) > 0 ? atoi(e) : 50) * 1000000ull;
    }
    if (last) {
        sum += d;
        n++;
        if (d > max) max = d, memcpy(worst_ns, frame_ns, sizeof worst_ns);
        if (d >= slow_ns)
            rt_log("perf: slow frame %llu: %.1f ms; blocked pipeline %.1f xlate %.1f texture %.1f astc %.1f fence %.1f pacing %.1f",
                   (unsigned long long)frame, d / 1e6, frame_ns[PERF_PIPE] / 1e6, frame_ns[PERF_XLATE] / 1e6,
                   frame_ns[PERF_TEX] / 1e6, frame_ns[PERF_ASTC] / 1e6, frame_ns[PERF_FENCE] / 1e6,
                   frame_ns[PERF_PACE] / 1e6);
    }
    memset(frame_ns, 0, sizeof frame_ns);
    last = now;
    if (!win_start) win_start = now;
    if (frame % 60) return;
    uint64_t span = now - win_start;
    LONG64 win[PERF_CNT_N];
    for (int i = 0; i < PERF_CNT_N; i++) {
        LONG64 v = __atomic_load_n(&counts[i], __ATOMIC_RELAXED);
        win[i] = v - win_prev[i];
        win_prev[i] = v;
    }
    LONG64 async = win[PERF_CNT_PIPE_ASYNC], skips = win[PERF_CNT_PIPE_SKIP], up = win[PERF_CNT_TEX_UP];
    LONG64 reup = win[PERF_CNT_TEX_REUP], sskip = win[PERF_CNT_SHADER_SKIP], waited = win[PERF_CNT_PIPE_WAIT];
    LONG64 oneshot = win[PERF_CNT_ONESHOT_WAIT];
    uint64_t shader_keys, shader_xlated;
    gfx_shader_counts(&shader_keys, &shader_xlated);
    if (n && span)
        rt_log("perf: frame %llu: interval avg %.2f ms max %.2f ms (%.1f fps); render thread blocked: pacing %.0f%% fence %.0f%% drain %.0f%% pipeline build %.0f%% shader xlate %.0f%% texture %.0f%% (astc %.0f%%); pipelines built in background %lld, draws skipped pending %lld, waited %lld, waiting for a shader %lld, one-shot waited %lld; textures uploaded %lld re-uploaded %lld; slowest frame: xlate %.1f ms texture %.1f ms (astc %.1f) pipeline %.1f ms fence %.1f ms; shaders: %llu keys, %llu translated; render busy avg %.2f ms; replay avg %.2f ms",
               (unsigned long long)frame, sum / 1e6 / n, max / 1e6, n * 1e9 / sum, 100.0 * wait_ns[PERF_PACE] / span,
               100.0 * wait_ns[PERF_FENCE] / span, 100.0 * wait_ns[PERF_DRAIN] / span,
               100.0 * wait_ns[PERF_PIPE] / span, 100.0 * wait_ns[PERF_XLATE] / span,
               100.0 * wait_ns[PERF_TEX] / span, 100.0 * wait_ns[PERF_ASTC] / span, (long long)async, (long long)skips, (long long)waited, (long long)sskip, (long long)oneshot,
               (long long)up, (long long)reup, worst_ns[PERF_XLATE] / 1e6, worst_ns[PERF_TEX] / 1e6,
               worst_ns[PERF_ASTC] / 1e6, worst_ns[PERF_PIPE] / 1e6, worst_ns[PERF_FENCE] / 1e6,
               (unsigned long long)shader_keys, (unsigned long long)shader_xlated,
               (span > wait_ns[PERF_PACE] ? span - wait_ns[PERF_PACE] : 0) / 1e6 / n,
               InterlockedExchange64(&replay_ns, 0) / 1e6 / n);
    if (n && span) perf_gpu_line(frame, sum / 1e6 / n, (double)wait_ns[PERF_PACE] / span, n);
    sum = max = n = 0;
    memset(wait_ns, 0, sizeof wait_ns);
    win_start = now;
}
