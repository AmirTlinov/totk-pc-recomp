// Adapted from MacNativeGames games/totk/runtime/threads.c @ b75be8c (MIT, Copyright (c) 2026 MacNativeGames
// contributors; itself from d2r-mobile dm/host/hle_thread.c), d2r-mobile dm/host/thread.c @ 53e7c5b (MIT, Copyright
// (c) 2026 jhaizhou) and LAS Recomp runtime/kernel.c @ da35b83 (MIT, Copyright (c) 2026 Peiikai).
// Threads, synchronization, virtual cores and fibers of the Horizon kernel.
//
// One guest thread = one Windows thread with a 256 MiB reserved host stack (every guest BL is a host call) and its own
// Cpu. The SDK's nn::os runs as lifted code; only the kernel under it is the host. Every wait hangs on one global lock
// G with one condition variable per thread; wake order is the kernel's: lower priority value first, then first come.
// Semantics follow the public Horizon kernel (KConditionVariable, KAddressArbiter, KSynchronization).
#include <windows.h>
#define __USE_MINGW_SETJMP_NON_SEH   // setjmp(b) = _setjmp(b, NULL): longjmp restores registers, no SEH unwinding
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "kernel.h"
#include "platform/platform.h"

#define R_OK 0u
#define R_TIMEOUT KERN_R_TIMEOUT
#define R_CANCELLED KERN_R_CANCELLED
#define R_BAD_HANDLE KERN_R_BAD_HANDLE
#define R_BAD_STATE KERN_R_BAD_STATE
#define R_INVALID_ENUM KERN_R_INVALID_ENUM
#define WAITERS_BIT KERN_WAITERS_BIT
#define H_CUR_THREAD KERN_H_CURRENT_THREAD

#define MAX_H 4096
#define NCORE 3
#define HOST_STACK (256ull << 20)        // reserved, committed on demand
#define TLR_SIZE 0x200
#define THREAD_OBJ_SIZE 0x400            // "current thread object" at TLR+0x1F8 (d2r-mobile thread.c)
#define TLS_SIZE 0x2000
#define MAIN_STACK (1ull << 20)          // main.npdm META+0x1C
#define MAIN_PRIO 44                     // META+0xE
#define MAIN_CORE 0                      // META+0xF
#define FIBER_MAX 1024

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W, O_SESSION = KERN_SESSION, O_TMEM = KERN_TMEM };
_Static_assert(O_EVENT_W < O_SESSION, "handle kinds");
enum { W_NONE = KERN_WAIT_NONE, W_LOCK = KERN_WAIT_LOCK, W_CONDVAR = KERN_WAIT_CONDVAR, W_ADDRESS = KERN_WAIT_ADDRESS,
       W_SYNC = KERN_WAIT_SYNC };

// Scheduler accounting states (TOTK_TRACE=sched).
enum { S_OFF, S_RUN, S_CORE, S_HOST, S_SLEEP, S_SYNC, S_CV, S_LOCK, S_ADDR, S_N };
static const char *const s_name[S_N] = {"off", "run", "core", "host", "sleep", "sync", "cv", "lock", "addr"};

typedef struct Event { int signaled, refs; } Event;
typedef struct Fiber Fiber;

struct RtThread {
    int preempt;               // first member: rt_dispatch reads it (host.h kern_preempt_pending)
    int host_depth;            // second member: host calls and SVCs in progress, while kern_host_track (host.h)
    uint32_t handle;
    uint64_t id;
    Cpu *cpu;                  // the thread's own context
    Cpu *cur;                  // the context running now: cpu, or the current fiber's
    uint64_t entry, arg, stack_top;
    int prio, core;
    uint64_t mask;
    int started, exited, closed, cancel, is_main;
    CONDITION_VARIABLE cv;
    // current wait
    int wkind, woke;
    uint64_t waddr, wkey;
    uint32_t wtag, wres;
    uint64_t wseq;             // registration order: equal priority wakes first come first
    RtThread *next;            // wait list
    RtThread *all;             // every thread
    // virtual core
    int cur_core;              // core held while running guest code, -1 while blocked
    uint64_t cseq;
    RtThread *cnext;
    uint8_t *tlr, *tls;
    jmp_buf exit_jmp;          // the thread start; ExitThread longjmps to it
    // fibers
    void *base_fiber;          // the host thread as a host fiber, after the first switch
    Fiber *fiber;              // current guest fiber, NULL on the thread's own context
    Fiber *leaving;            // fiber just switched away from, released by whoever resumes next
    int exit_pending;          // ExitThread on a fiber: unwind once back on the thread's own context
    HANDLE timer;              // high-resolution waitable timer for SleepThread
    // TOTK_TRACE=sched accounting (holding G)
    DWORD tid;                 // host thread id
    int st;                    // S_* state since st_since
    uint64_t st_since, st_ns[S_N], nblk, npre, pre_since, cpu_last;
    uint64_t wobj;             // what the current wait is on: owner handle, key, address or first handle
    // TOTK_CORE_DIAG (holding G, except dg_host_end, which only the thread itself writes)
    uint64_t dg_since;         // as a waiter: core wait start
    int dg_cls;                // as a waiter: DG_* class at the start of the wait
    RtThread *dg_target;       // as a waiter: the owner asked to give its core up for it
    uint64_t dg_kick;          // as an owner: the preempt request
    uint64_t dg_host_end;      // as an owner: the return from the host call or SVC it was in at the request
    uint64_t dg_pc, dg_lr;     // as an owner: guest pc and lr at the checkpoint that gave the core up
    int dg_host_kick;          // as an owner: in a host call or SVC at the request
    struct DgRel { uint64_t kick, host_end, pc, lr; int host, pre, prio; uint32_t h; } dg_rel;   // as a waiter: the
                                                                                                 // owner that released
};

static SRWLOCK G = SRWLOCK_INIT;
static struct { int type; void *obj; } g_h[MAX_H];
static _Thread_local RtThread *tcur;
_Static_assert(offsetof(RtThread, preempt) == 0, "host.h kern_preempt_pending reads the first int");
_Static_assert(offsetof(RtThread, host_depth) == sizeof(int), "host.h kern_host_depth reads the second int");
static RtThread *g_all, *g_wait, *g_main;
static uint64_t g_next_id = 0x51, g_wseq, g_created;
static uint64_t g_main_stack;

static void lock(void) { AcquireSRWLockExclusive(&G); }
static void unlock(void) { ReleaseSRWLockExclusive(&G); }

uint64_t kern_now_ns(void) { return plat_now_ns(); }

// ---- TOTK_TRACE=sched accounting (holding G)
#define RW_N 32
static RtThread *g_render;                               // the thread that presents (sched trace, core diag)
static struct { int st; uint64_t obj, ns; } g_rw[RW_N];   // the render thread's waits by object
static uint64_t g_pre_req, g_pre_n, g_pre_ns, g_pre_max, g_pre_stale;  // preempt requests; releases after one and their latency
static uint64_t g_sync_calls, g_sync_blocks;              // WaitSynchronization calls and the blocks inside them

static void render_wait(int st, uint64_t obj, uint64_t ns) {
    for (int i = 0; i < RW_N; i++)
        if (!g_rw[i].ns || (g_rw[i].st == st && g_rw[i].obj == obj)) {
            g_rw[i].st = st;
            g_rw[i].obj = obj;
            g_rw[i].ns += ns;
            return;
        }
}

// Close the current state's interval and enter st.
static void acct(RtThread *t, int st) {
    if (!(rt_trace & RT_TRACE_SCHED)) return;
    uint64_t now = kern_now_ns();
    if (t->st_since) {
        t->st_ns[t->st] += now - t->st_since;
        if (t == g_render && t->st >= S_HOST) render_wait(t->st, t->wobj, now - t->st_since);
    }
    t->st = st;
    t->st_since = now;
}

static int st_of(int kind) {
    return kind == W_LOCK ? S_LOCK : kind == W_CONDVAR ? S_CV : kind == W_ADDRESS ? S_ADDR : S_SYNC;
}

// ---- handles (caller holds G)
static uint32_t h_new(int type, void *obj) {
    for (int i = 1; i < MAX_H; i++)
        if (!g_h[i].type) {
            g_h[i].type = type;
            g_h[i].obj = obj;
            return 0x1000 + i;
        }
    rt_fatal("kernel: handle table full (%d handles)", MAX_H);
}

static void *h_peek(uint32_t h, int type) {
    if (h == H_CUR_THREAD && type == O_THREAD) return tcur;
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}

static void *h_get(uint32_t h, int type) {
    void *o = h_peek(h, type);
    if (!o) rt_log("kernel: invalid handle %#x (want kind %d)", h, type);
    return o;
}

static RtThread *cur(uint32_t svc) {
    if (!tcur) rt_fatal("kernel: svc %#x on a host thread (no guest thread)", svc);
    return tcur;
}

// ---- wait primitives (holding G)
static void wait_unlink(RtThread *t) {
    for (RtThread **p = &g_wait; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            break;
        }
    t->next = NULL;
}

// The waiter to wake in kernel order. Lock waits match by lock address, condvar waits by key, arbiter waits by
// address.
static RtThread *pick(int kind, uint64_t a) {
    RtThread *b = NULL;
    for (RtThread *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == W_CONDVAR ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}

static void wake(RtThread *t, uint32_t res) {
    wait_unlink(t);
    t->wkind = W_NONE;
    t->woke = 1;
    t->wres = res;
    WakeConditionVariable(&t->cv);
}

static void wake_sync_waiters(void) {
    for (RtThread *w = g_wait; w;) {
        RtThread *n = w->next;
        if (w->wkind == W_SYNC) wake(w, (uint32_t)-1);
        w = n;
    }
}

// SleepConditionVariableSRW counts milliseconds: round up (late is legal, early is not) and re-check the deadline
// against the performance counter, because the wait may also end early.
static uint32_t block_inner(RtThread *t, int kind, int64_t ns) {
    t->wkind = kind;
    t->woke = 0;
    t->wseq = ++g_wseq;
    t->next = g_wait;
    g_wait = t;
    if (ns == 0) {
        wait_unlink(t);
        t->wkind = W_NONE;
        return R_TIMEOUT;
    }
    uint64_t now = kern_now_ns();
    int forever = ns < 0 || (uint64_t)ns > UINT64_MAX - now;
    uint64_t deadline = forever ? 0 : now + (uint64_t)ns;
    while (!t->woke) {
        if (forever) {
            SleepConditionVariableSRW(&t->cv, &G, INFINITE, 0);
            continue;
        }
        now = kern_now_ns();
        if (now >= deadline) {
            wait_unlink(t);
            t->wkind = W_NONE;
            return R_TIMEOUT;
        }
        uint64_t ms = (deadline - now + 999999) / 1000000;
        SleepConditionVariableSRW(&t->cv, &G, ms > 0xFFFFFFF0u ? 0xFFFFFFF0u : (DWORD)ms, 0);
    }
    return t->wres;
}

// ---- virtual cores
// Horizon runs at most one thread per core, and TotK keys per-core data (allocators, job queues) by the current core
// number without locks. A guest thread therefore holds one of the three application cores while it runs guest code
// and gives it up only where Horizon would switch threads (waits, sleep, yield, exit, blocking host calls); a free
// core goes to the best waiting thread. A waiter better than a holder asks one holder to give its core up (core_kick),
// which it does at its next SVC or indirect call (kern_checkpoint, rt_dispatch), as Horizon runs the better thread at
// once; guest code without either keeps the core until it reaches one. TOTK_FREE_CORES=1 turns the gate off.
static RtThread *g_core_owner[NCORE], *g_core_wait;
static int g_cores_on = -1, g_held_max;

static void diag_init(void);

static int cores_enabled(void) {
    if (g_cores_on < 0) {
        g_cores_on = cfg_get("TOTK_FREE_CORES") ? 0 : 1;
        diag_init();
    }
    return g_cores_on;
}

static int core_allowed(const RtThread *t, int c) { return (int)((t->mask >> c) & 1); }

// ---- TOTK_CORE_DIAG=1: why ready threads wait for a virtual core, and for how long
// Each core wait is split into classes: the owner asked to give the core up ran game code (DG_GAME, the part an
// asynchronous preemption would remove), was in a host call or SVC that kept the core (DG_HOST), every owner the waiter
// may displace was of better or equal priority (DG_EQ: Horizon waits too), or the worse owners were claimed by better
// waiters (DG_NOCORE). Per 60 presents: time per frame and a wait-length histogram per class, the owners that cost
// most; a line per present over 25 ms; a line per wait with over 2 ms of DG_GAME (where the owner reached its
// checkpoint), and one host RIP sample of the owner per such wait (exe RVA, for totk.map).
enum { DG_GAME, DG_HOST, DG_EQ, DG_NOCORE, DG_N };
static const char *const dg_name[DG_N] = {"game", "host", "eq", "nocore"};
#define DG_NB 8   // wait lengths: < 0.05, 0.25, 1, 2, 4, 8, 16 ms, longer
static const uint64_t dg_edge[DG_NB - 1] = {50000, 250000, 1000000, 2000000, 4000000, 8000000, 16000000};
#define DG_LONG_LINES 400
#define DG_SAMPLES 600
#define DG_OWN 32
int kern_host_track;
static int g_diag = -1;
typedef struct {
    uint64_t ns[DG_N], n[DG_N], hist[DG_N][DG_NB];                    // this window
    struct { uint32_t h; int prio, cls; uint64_t ns; } own[DG_OWN];   // this window: owners by class
    uint64_t long_frames, long_game_frames;
} DgWindow;
static DgWindow g_dgw;
static uint64_t g_dgf[DG_N], g_dgfr[DG_N];   // this frame: all threads, the render thread
static uint64_t g_dg_samples, g_dg_lines, g_sync36, g_sync36_flag;

static void diag_init(void) {
    if (g_diag >= 0) return;
    const char *e = cfg_get("TOTK_CORE_DIAG");
    g_diag = e && *e && strcmp(e, "0");
    if (g_diag) {
        kern_host_track = 1;
        rt_log("core-diag: on (TOTK_CORE_DIAG)");
    }
}

int kern_core_diag_on(void) { return g_diag > 0; }

// A tracked host call or SVC returned to depth 0 with a preempt request pending (host.h); on the thread itself.
void kern_host_left(void *thread) {
    RtThread *t = thread;
    if (g_diag > 0 && t->dg_kick && t->dg_host_kick && !t->dg_host_end) t->dg_host_end = kern_now_ns();
}

static int dg_in_host(const RtThread *o) { return __atomic_load_n(&o->host_depth, __ATOMIC_RELAXED) > 0; }

static void dg_add(const RtThread *w, int cls, uint64_t ns, uint32_t oh, int oprio) {
    if (!ns) return;
    g_dgw.ns[cls] += ns;
    g_dgf[cls] += ns;
    if (w == g_render) g_dgfr[cls] += ns;
    if (!oh) return;
    int i = 0;
    for (; i < DG_OWN && g_dgw.own[i].ns; i++)
        if (g_dgw.own[i].h == oh && g_dgw.own[i].cls == cls) break;
    if (i == DG_OWN) return;
    g_dgw.own[i].h = oh;
    g_dgw.own[i].prio = oprio;
    g_dgw.own[i].cls = cls;
    g_dgw.own[i].ns += ns;
}

// The waiter's class at the start of its wait, after core_kick claimed (or did not claim) an owner for it.
static int dg_class0(const RtThread *t) {
    if (t->dg_target) return dg_in_host(t->dg_target) ? DG_HOST : DG_GAME;
    for (int i = 0; i < NCORE; i++)
        if (core_allowed(t, i) && g_core_owner[i] && g_core_owner[i]->prio > t->prio) return DG_NOCORE;
    return DG_EQ;
}

// The waiter got a core: split [dg_since, now] by what the owner that released it did after its preempt request.
static void dg_done(RtThread *t) {
    uint64_t now = kern_now_ns(), t0 = t->dg_since, w = now - t0;
    t->dg_since = 0;
    if (!t0) return;
    const struct DgRel *r = &t->dg_rel;
    uint64_t part[DG_N] = {0};
    uint32_t oh = t->dg_target ? t->dg_target->handle : r->h;
    int oprio = t->dg_target ? t->dg_target->prio : r->prio;
    if (r->pre && r->kick) {
        uint64_t k = r->kick > t0 ? (r->kick < now ? r->kick : now) : t0;
        part[t->dg_cls] += k - t0;
        if (r->host) {
            uint64_t he = r->host_end > k && r->host_end < now ? r->host_end : now;
            part[DG_HOST] += he - k;
            part[DG_GAME] += now - he;
        } else
            part[DG_GAME] += now - k;
        oh = r->h;
        oprio = r->prio;
    } else
        part[t->dg_cls] += w;
    int top = 0;
    for (int c = 0; c < DG_N; c++) {
        dg_add(t, c, part[c], oh, oprio);
        if (part[c] > part[top]) top = c;
    }
    int b = 0;
    while (b < DG_NB - 1 && w >= dg_edge[b]) b++;
    g_dgw.n[top]++;
    g_dgw.hist[top][b]++;
    if (part[DG_GAME] >= 2000000 && g_dg_lines < DG_LONG_LINES) {
        g_dg_lines++;
        char a[128], l[128];
        rt_log("core-diag: wait %.2f ms (game %.2f host %.2f): %#x p%d mask %llx; released by %#x p%d%s at %s lr %s",
               w / 1e6, part[DG_GAME] / 1e6, part[DG_HOST] / 1e6, t->handle, t->prio, (unsigned long long)t->mask, r->h,
               r->prio, r->pre ? " (preempted)" : "", r->pc ? ldr_describe(r->pc, a, sizeof a) : "-",
               r->lr ? ldr_describe(r->lr, l, sizeof l) : "-");
    }
    t->dg_target = NULL;
    memset(&t->dg_rel, 0, sizeof t->dg_rel);
}

// Once per long DG_GAME wait: where the owner's host thread runs (exe RVA). Holding G; the owner is resumed before
// anything that could take a lock it holds.
static void dg_sample(RtThread *t) {
    RtThread *o = t->dg_target;
    if (!o || o->cur_core < 0 || !o->tid || g_dg_samples >= DG_SAMPLES) return;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, o->tid);
    if (!h) return;
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    int depth = 0, ok = 0;
    if (SuspendThread(h) != (DWORD)-1) {
        ok = GetThreadContext(h, &ctx);
        depth = __atomic_load_n(&o->host_depth, __ATOMIC_RELAXED);
        ResumeThread(h);
    }
    CloseHandle(h);
    if (!ok) return;
    g_dg_samples++;
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    rt_log("core-diag: sample owner %#x p%d depth %d rip exe+%#llx (waiter %#x p%d, %.2f ms)", o->handle, o->prio, depth,
           (unsigned long long)(ctx.Rip - base), t->handle, t->prio, (kern_now_ns() - t->dg_since) / 1e6);
}


// perf_present, every present: a line for a present over 25 ms, the 60-present summary.
void kern_core_diag_frame(uint64_t frame, uint64_t interval_ns) {
    if (g_diag <= 0) return;
    DgWindow w;
    uint64_t f[DG_N], fr[DG_N], s36, s36f;
    lock();
    g_render = tcur;
    memcpy(f, g_dgf, sizeof f);
    memcpy(fr, g_dgfr, sizeof fr);
    memset(g_dgf, 0, sizeof g_dgf);
    memset(g_dgfr, 0, sizeof g_dgfr);
    int lng = interval_ns > 25000000ull;
    if (lng) {
        g_dgw.long_frames++;
        if (f[DG_GAME] > 1000000) g_dgw.long_game_frames++;
    }
    int rep = frame && frame % 60 == 0;
    if (rep) {
        w = g_dgw;
        memset(&g_dgw, 0, sizeof g_dgw);
    }
    s36 = g_sync36;
    s36f = g_sync36_flag;
    unlock();
    if (lng)
        rt_log("core-diag: frame %llu: %.1f ms; core wait game %.2f host %.2f eq %.2f nocore %.2f ms; render thread game "
               "%.2f host %.2f eq %.2f nocore %.2f ms",
               (unsigned long long)frame, interval_ns / 1e6, f[0] / 1e6, f[1] / 1e6, f[2] / 1e6, f[3] / 1e6,
               fr[0] / 1e6, fr[1] / 1e6, fr[2] / 1e6, fr[3] / 1e6);
    if (!rep) return;
    char s[1024];
    int o = 0;
    for (int c = 0; c < DG_N; c++) {
        o += snprintf(s + o, sizeof s - o, "%s %.3f ms/frame %llu waits [", dg_name[c], w.ns[c] / 60e6,
                      (unsigned long long)w.n[c]);
        for (int b = 0; b < DG_NB; b++)
            o += snprintf(s + o, sizeof s - o, "%s%llu", b ? " " : "", (unsigned long long)w.hist[c][b]);
        o += snprintf(s + o, sizeof s - o, "]%s", c < DG_N - 1 ? "; " : "");
    }
    rt_log("core-diag: frames %llu-%llu: %s; long frames %llu (game wait > 1 ms in %llu); SVC 0x36 %llu (flag set %llu)",
           (unsigned long long)frame - 59, (unsigned long long)frame, s, (unsigned long long)w.long_frames,
           (unsigned long long)w.long_game_frames, (unsigned long long)s36, (unsigned long long)s36f);
    o = 0;
    for (int k = 0; k < 6; k++) {
        int bi = -1;
        for (int i = 0; i < DG_OWN; i++)
            if (w.own[i].ns && (bi < 0 || w.own[i].ns > w.own[bi].ns)) bi = i;
        if (bi < 0) break;
        o += snprintf(s + o, sizeof s - o, " %#x p%d %s %.3f;", w.own[bi].h, w.own[bi].prio, dg_name[w.own[bi].cls],
                      w.own[bi].ns / 60e6);
        w.own[bi].ns = 0;
    }
    if (o) rt_log("core-diag: owners (ms/frame):%s", s);
}

static void core_count(void) {
    int n = 0;
    for (int i = 0; i < NCORE; i++) n += g_core_owner[i] != NULL;
    if (n > g_held_max) g_held_max = n;
}

static struct DgRel g_dg_rel;   // TOTK_CORE_DIAG: the owner core_put is releasing, for core_grant

static void core_grant(int c) {
    RtThread *b = NULL, **bp = NULL;
    for (RtThread **p = &g_core_wait; *p; p = &(*p)->cnext)
        if (core_allowed(*p, c) && (!b || (*p)->prio < b->prio || ((*p)->prio == b->prio && (*p)->cseq < b->cseq))) {
            b = *p;
            bp = p;
        }
    if (!b) return;
    *bp = b->cnext;
    b->cnext = NULL;
    b->cur_core = c;
    g_core_owner[c] = b;
    core_count();
    if (g_diag > 0) b->dg_rel = g_dg_rel;
    WakeConditionVariable(&b->cv);
}

static void core_put(RtThread *t) {
    if (t->cur_core < 0) return;
    int c = t->cur_core;
    if (t->preempt && t->pre_since) {   // sched trace: time from the preempt request to this release
        uint64_t d = kern_now_ns() - t->pre_since;
        g_pre_n++;
        g_pre_ns += d;
        if (d > g_pre_max) g_pre_max = d;
        t->npre++;
    }
    t->pre_since = 0;
    if (g_diag > 0) {
        g_dg_rel = (struct DgRel){.kick = t->dg_kick, .host_end = t->dg_host_end, .pc = t->dg_pc, .lr = t->dg_lr,
                                  .host = t->dg_host_kick, .pre = t->preempt, .prio = t->prio, .h = t->handle};
        t->dg_kick = t->dg_host_end = t->dg_pc = t->dg_lr = 0;
        t->dg_host_kick = 0;
    }
    t->cur_core = -1;
    t->preempt = 0;
    g_core_owner[c] = NULL;
    core_grant(c);
}

// Whether a waiter better than t may run on core c.
static int core_better_waiter(const RtThread *t, int c) {
    for (const RtThread *w = g_core_wait; w; w = w->cnext)
        if (w->prio < t->prio && core_allowed(w, c)) return 1;
    return 0;
}

// Asks holders to give their cores up, one holder per waiter: waiters best first, each claims the worst holder worse
// than itself on a core it may run on that no better waiter claimed (a request already pending is reused). Asking
// every worse holder made several drop their cores for one waiter; the spare cores went to worse waiters, which asked
// again (build/v121-dpre/NOTES.md).
static void core_kick(void) {
    RtThread *w[64];
    int n = 0, claimed = 0;
    for (RtThread *p = g_core_wait; p && n < 64; p = p->cnext) {
        int i = n++;
        for (; i > 0 && (w[i - 1]->prio > p->prio || (w[i - 1]->prio == p->prio && w[i - 1]->cseq > p->cseq)); i--)
            w[i] = w[i - 1];
        w[i] = p;
    }
    for (int k = 0; k < n && claimed != (1 << NCORE) - 1; k++) {
        int v = -1;
        for (int i = 0; i < NCORE; i++) {
            RtThread *o = g_core_owner[i];
            if ((claimed >> i & 1) || !core_allowed(w[k], i) || !o || o->prio <= w[k]->prio) continue;
            RtThread *b = v < 0 ? NULL : g_core_owner[v];
            if (!b || o->prio > b->prio || (o->prio == b->prio && o->preempt && !b->preempt)) v = i;
        }
        if (v < 0) continue;
        claimed |= 1 << v;
        RtThread *o = g_core_owner[v];
        w[k]->dg_target = o;
        if (o->preempt) continue;
        if (rt_trace & RT_TRACE_SCHED) {
            o->pre_since = kern_now_ns();
            g_pre_req++;
        }
        if (g_diag > 0) {
            o->dg_kick = kern_now_ns();
            o->dg_host_kick = dg_in_host(o);
            o->dg_host_end = 0;
        }
        __atomic_store_n(&o->preempt, 1, __ATOMIC_RELEASE);
    }
}

static void core_take(RtThread *t) {
    if (t->cur_core >= 0) return;
    if (!cores_enabled()) {
        acct(t, S_RUN);
        return;
    }
    if (!(t->mask & ((1u << NCORE) - 1)))
        rt_fatal("kernel: thread %#x core mask %#llx has no application core", t->handle, (unsigned long long)t->mask);
    int c = t->core >= 0 && t->core < NCORE && core_allowed(t, t->core) && !g_core_owner[t->core] ? t->core : -1;
    for (int i = 0; c < 0 && i < NCORE; i++)
        if (core_allowed(t, i) && !g_core_owner[i]) c = i;
    if (c >= 0) {
        t->cur_core = c;
        g_core_owner[c] = t;
        core_count();
        acct(t, S_RUN);
        return;
    }
    acct(t, S_CORE);
    t->cseq = ++g_wseq;
    t->cnext = g_core_wait;
    g_core_wait = t;
    if (g_diag > 0) {
        t->dg_since = kern_now_ns();
        t->dg_target = NULL;
        memset(&t->dg_rel, 0, sizeof t->dg_rel);
    }
    core_kick();
    if (g_diag > 0) {
        t->dg_cls = dg_class0(t);
        int sampled = 0;
        while (t->cur_core < 0) {
            if (sampled || t->dg_cls != DG_GAME) {
                SleepConditionVariableSRW(&t->cv, &G, INFINITE, 0);
                continue;
            }
            SleepConditionVariableSRW(&t->cv, &G, 2, 0);
            if (t->cur_core < 0 && kern_now_ns() - t->dg_since >= 2000000) {
                sampled = 1;
                dg_sample(t);
            }
        }
        dg_done(t);
    }
    while (t->cur_core < 0) SleepConditionVariableSRW(&t->cv, &G, INFINITE, 0);
    acct(t, S_RUN);
}

static uint32_t block(RtThread *t, int kind, int64_t ns) {
    if (ns == 0) return block_inner(t, kind, ns);
    core_put(t);
    acct(t, st_of(kind));
    t->nblk++;
    uint32_t r = block_inner(t, kind, ns);
    core_take(t);
    return r;
}

int kern_core_away(void) {
    RtThread *t = tcur;
    if (!t || t->cur_core < 0) return 0;
    lock();
    core_put(t);
    acct(t, S_HOST);
    if (rt_trace & RT_TRACE_SCHED)   // the caller as an exe RVA (totk.map)
        t->wobj = (uintptr_t)__builtin_return_address(0) - (uintptr_t)GetModuleHandleW(NULL);
    unlock();
    return 1;
}

void kern_core_back(int *away) {
    RtThread *t = tcur;
    if (!*away || !t) return;
    lock();
    core_take(t);
    unlock();
    *away = 0;
}

// A pending preempt request, at an SVC or an indirect call. The core goes over only if a better thread still waits
// for it, so core_put grants it to a waiter better than t; a stale request (that waiter got another core) is dropped.
void kern_checkpoint(void) {
    RtThread *t = tcur;
    if (!t || !__atomic_load_n(&t->preempt, __ATOMIC_RELAXED)) return;
    lock();
    if (g_diag > 0 && t->cur) {
        t->dg_pc = t->cur->pc;
        t->dg_lr = t->cur->x[30];
    }
    if (t->cur_core >= 0 && core_better_waiter(t, t->cur_core)) {
        core_put(t);   // clears preempt (after the sched trace reads it)
        core_take(t);
    } else {
        t->preempt = 0;
        t->pre_since = 0;
        t->dg_kick = t->dg_host_end = 0;
        t->dg_host_kick = 0;
        g_pre_stale++;
    }
    unlock();
}

void kern_core_stats(int *held_now, int *held_max) {
    lock();
    int n = 0;
    for (int i = 0; i < NCORE; i++) n += g_core_owner[i] != NULL;
    *held_now = n;
    *held_max = g_held_max;
    unlock();
}

void kern_core_stats_reset(void) {
    lock();
    g_held_max = 0;
    core_count();
    unlock();
}

// TOTK_TRACE=sched: every 120 presents, the time split of the busiest threads (running, waiting for a core, host
// calls) and of the render thread, the render thread's longest waits by object, and the preemption latency.
#define REP_N 8
void kern_sched_report(uint64_t frame) {
    static uint64_t last;
    typedef struct { uint32_t h; int prio, render; uint64_t mask, entry, ns[S_N], nblk, npre, cpu; } Row;
    Row rows[REP_N + 1];
    struct { int st; uint64_t obj, ns; } rw[3] = {0};
    int nr = 0;
    RtThread *self = tcur;
    lock();
    if (g_render != self) {
        g_render = self;
        memset(g_rw, 0, sizeof g_rw);
    }
    if (frame % 120) {
        unlock();
        return;
    }
    uint64_t now = kern_now_ns(), span = last ? now - last : 0;
    last = now;
    RtThread *top[REP_N + 1] = {0};
    uint64_t busy[REP_N + 1] = {0}, all_run = 0, all_core = 0;
    for (RtThread *t = g_all; t; t = t->all) {
        acct(t, t->st);   // close the open interval
        uint64_t b = t->st_ns[S_RUN] + t->st_ns[S_CORE] + t->st_ns[S_HOST];
        all_run += t->st_ns[S_RUN];
        all_core += t->st_ns[S_CORE];
        if (t == self) continue;
        for (int i = 0; i < REP_N; i++)
            if (!top[i] || b > busy[i]) {
                memmove(top + i + 1, top + i, (REP_N - 1 - i) * sizeof *top);
                memmove(busy + i + 1, busy + i, (REP_N - 1 - i) * sizeof *busy);
                top[i] = t;
                busy[i] = b;
                break;
            }
    }
    if (self) {   // the render thread first
        memmove(top + 1, top, REP_N * sizeof *top);
        top[0] = self;
    }
    for (int i = 0; i <= REP_N && top[i]; i++) {
        RtThread *t = top[i];
        Row *r = &rows[nr++];
        r->h = t->handle;
        r->prio = t->prio;
        r->render = t == self;
        r->mask = t->mask;
        r->entry = t->entry;
        memcpy(r->ns, t->st_ns, sizeof r->ns);
        r->nblk = t->nblk;
        r->npre = t->npre;
        r->cpu = 0;
        HANDLE th = t->tid ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, t->tid) : NULL;
        FILETIME a, b, k, u;
        if (th && GetThreadTimes(th, &a, &b, &k, &u)) {
            uint64_t c = (((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) +
                          ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime)) * 100;
            r->cpu = t->cpu_last ? c - t->cpu_last : 0;
            t->cpu_last = c;
        }
        if (th) CloseHandle(th);
    }
    for (int i = 0; i < RW_N && g_rw[i].ns; i++)
        for (int j = 0; j < 3; j++)
            if (g_rw[i].ns > rw[j].ns) {
                memmove(rw + j + 1, rw + j, (2 - j) * sizeof *rw);
                rw[j].st = g_rw[i].st;
                rw[j].obj = g_rw[i].obj;
                rw[j].ns = g_rw[i].ns;
                break;
            }
    uint64_t pre_stale = g_pre_stale, pre_req = g_pre_req, pre_n = g_pre_n, pre_ns = g_pre_ns, pre_max = g_pre_max;
    uint64_t sc = g_sync_calls, sb = g_sync_blocks;
    for (RtThread *t = g_all; t; t = t->all) {
        memset(t->st_ns, 0, sizeof t->st_ns);
        t->nblk = t->npre = 0;
    }
    memset(g_rw, 0, sizeof g_rw);
    g_pre_req = g_pre_n = g_pre_ns = g_pre_max = g_pre_stale = 0;
    g_sync_calls = g_sync_blocks = 0;
    unlock();
    if (!span) return;
    rt_log("sched: frame %llu: %.2f s, cores busy %.2f of %d, core wait %.2f thread-s, preempt req %llu released %llu "
           "avg %.2f ms max %.2f ms, WaitSynchronization %llu calls %llu blocks, stale preempt %llu%s",
           (unsigned long long)frame, span / 1e9, (double)all_run / span, NCORE, all_core / 1e9,
           (unsigned long long)pre_req, (unsigned long long)pre_n, pre_n ? pre_ns / 1e6 / pre_n : 0.0, pre_max / 1e6,
           (unsigned long long)sc, (unsigned long long)sb, (unsigned long long)pre_stale, self ? "" : " (render thread is not a guest thread)");
    for (int i = 0; i < nr; i++) {
        Row *r = &rows[i];
        char d[128], s[256];
        int o = 0;
        for (int k = S_RUN; k < S_N; k++)
            o += snprintf(s + o, sizeof s - o, " %s %.0f%%", s_name[k], 100.0 * r->ns[k] / span);
        ldr_describe(r->entry, d, sizeof d);
        rt_log("sched: %s%#x p%d m%llx%s cpu %.0f%% blocks %llu preempted %llu (%s)", r->render ? "render " : "",
               r->h, r->prio, (unsigned long long)r->mask, s, 100.0 * r->cpu / span, (unsigned long long)r->nblk,
               (unsigned long long)r->npre, r->entry ? d : "main");
    }
    for (int j = 0; j < 3 && rw[j].ns; j++)
        rt_log("sched: render wait %d: %s on %s%#llx: %.1f%%", j + 1, s_name[rw[j].st], rw[j].st == S_HOST ? "exe+" : "",
               (unsigned long long)rw[j].obj,
               100.0 * rw[j].ns / span);
}

// ---- locks: tag word = owner handle | WAITERS_BIT
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)(uintptr_t)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)(uintptr_t)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) {
    return __atomic_compare_exchange_n((uint32_t *)(uintptr_t)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// Release the lock at addr: hand it to the best waiter, or clear it.
static void lock_handoff(uint64_t addr) {
    RtThread *w = pick(W_LOCK, addr);
    int more = 0;
    if (!w) {
        st32(addr, 0);
        return;
    }
    for (RtThread *t = g_wait; t; t = t->next)
        if (t != w && t->wkind == W_LOCK && t->waddr == addr) {
            more = 1;
            break;
        }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}

// A condvar waiter takes its lock: directly if free (and wakes), else it becomes a lock waiter behind the others.
static void lock_acquire_for(RtThread *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) {
            if (cas32(addr, &v, tag)) {
                wake(t, 0);
                return;
            }
            continue;
        }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    acct(t, S_LOCK);
    t->wobj = v & ~WAITERS_BIT;
    t->wkind = W_LOCK;
    t->waddr = addr;
    t->wtag = tag;
    t->wseq = ++g_wseq;
}

// ---- threads
static RtThread *thr_new(int prio, int core, uint64_t mask) {
    RtThread *t = calloc(1, sizeof *t);
    Cpu *c = _aligned_malloc(sizeof(Cpu), 64);
    if (!t || !c) rt_fatal("kernel: thread allocation");
    memset(c, 0, sizeof *c);
    InitializeConditionVariable(&t->cv);
    t->prio = prio;
    t->core = core;
    t->mask = mask;
    t->cur_core = -1;
    // TLR (0x200) and the "current thread object" the SDK reads through TLR+0x1F8, in one guest-visible block.
    t->tlr = mem_kernel_alloc(TLR_SIZE + THREAD_OBJ_SIZE, TLR_SIZE);
    *(uint64_t *)(t->tlr + 0x1F8) = (uint64_t)(uintptr_t)(t->tlr + TLR_SIZE);
    t->tls = mem_kernel_alloc(TLS_SIZE, 16);
    c->tpidrro = (uint64_t)(uintptr_t)t->tlr;
    c->tpidr = (uint64_t)(uintptr_t)t->tls;
    c->excl_addr = ~0ull;
    c->thread = t;
    t->cpu = t->cur = c;
    return t;
}

static void thr_link(RtThread *t) {   // holding G
    t->handle = h_new(O_THREAD, t);
    t->id = g_next_id++;
    t->all = g_all;
    g_all = t;
    g_created++;
}

static void thr_free(RtThread *t) {   // holding G; t has exited and its handle is closed
    for (RtThread **p = &g_all; *p; p = &(*p)->all)
        if (*p == t) {
            *p = t->all;
            break;
        }
    mem_kernel_free(t->tlr, TLR_SIZE + THREAD_OBJ_SIZE);
    mem_kernel_free(t->tls, TLS_SIZE);
    _aligned_free(t->cpu);
    free(t);
}

// The end of every guest thread, on its own host context after the unwind: give the core up, signal the thread
// object, free it if its handle is already closed. t is not touched after the unlock.
static void thread_finish(RtThread *t) {
    if (t->timer) CloseHandle(t->timer);
    t->timer = NULL;
    if (t->base_fiber) ConvertFiberToThread();
    t->base_fiber = NULL;
    tcur = NULL;
    lock();
    core_put(t);
    acct(t, S_OFF);
    t->exited = 1;
    wake_sync_waiters();
    if (t->closed) thr_free(t);
    unlock();
}

// ExitThread: back to the thread start with a non-unwinding longjmp; no SEH unwinding through lifted frames.
// (__builtin_setjmp is not usable: on Win64 LLVM saves the frame address, which is not rbp when the frame register
// is offset, and the landing code then reads its locals through the wrong base.)
static __attribute__((noreturn)) void exit_now(RtThread *t) { longjmp(t->exit_jmp, 1); }

static DWORD WINAPI thread_main(void *p) {
    RtThread *t = p;
    tcur = t;
    t->tid = GetCurrentThreadId();
    lock();
    while (!t->started) SleepConditionVariableSRW(&t->cv, &G, INFINITE, 0);
    core_take(t);
    unlock();
    Cpu *c = t->cpu;
    c->sp = t->stack_top & ~15ull;
    if (!setjmp(t->exit_jmp)) {
        rt_call(c, t->entry, 1, t->arg);
        char d[128];
        rt_fatal("guest thread %#x: entry %s returned (Horizon threads end with ExitThread)", t->handle,
                 ldr_describe(t->entry, d, sizeof d));
    }
    thread_finish(tcur);
    return 0;
}

uint64_t kern_threads_created(void) {
    lock();
    uint64_t n = g_created;
    unlock();
    return n;
}

Cpu *kern_cpu(void) { return tcur ? tcur->cur : NULL; }

uint32_t kern_main_thread(Cpu **cpu) {
    if (tcur) {
        if (!tcur->is_main) rt_fatal("kern_main_thread on guest thread %#x, not the main thread", tcur->handle);
        *cpu = tcur->cpu;
        return tcur->handle;
    }
    if (g_main) rt_fatal("kern_main_thread: the main thread already runs on another host thread");
    const RtGuestLayout *L = mem_layout();
    uint64_t stack = L->stack.base + L->stack.size - MAIN_STACK;
    if (!VirtualAlloc((void *)(uintptr_t)stack, MAIN_STACK, MEM_COMMIT, PAGE_READWRITE))
        rt_fatal("kernel: commit the main thread stack at %#llx failed (error %lu)", (unsigned long long)stack,
                 GetLastError());
    RtThread *t = thr_new(MAIN_PRIO, MAIN_CORE, 7);
    t->started = 1;
    t->is_main = 1;
    t->stack_top = stack + MAIN_STACK;
    t->cpu->sp = t->stack_top;
    t->tid = GetCurrentThreadId();
    lock();
    thr_link(t);
    g_main = t;
    g_main_stack = stack;
    core_take(t);
    unlock();
    tcur = t;
    *cpu = t->cpu;
    return t->handle;
}

void kern_main_stack(uint64_t *base, uint64_t *size) {
    uint64_t b = __atomic_load_n(&g_main_stack, __ATOMIC_ACQUIRE);
    *base = b;
    *size = b ? MAIN_STACK : 0;
}

static DWORD WINAPI guest_main(void *p) {
    void (*fn)(Cpu *) = (void (*)(Cpu *))p;
    Cpu *c;
    kern_main_thread(&c);
    if (!setjmp(tcur->exit_jmp)) fn(c);
    thread_finish(tcur);
    return 0;
}

void kern_run_on_guest_main(void (*fn)(Cpu *c)) {
    HANDLE h = CreateThread(NULL, HOST_STACK, guest_main, (void *)fn, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) rt_fatal("kernel: CreateThread for the guest main thread failed (error %lu)", GetLastError());
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
}

static void sleep_ns(RtThread *t, int64_t ns) {
    if (!t->timer) {
        t->timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!t->timer) t->timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        if (!t->timer) rt_fatal("kernel: CreateWaitableTimerExW failed (error %lu)", GetLastError());
    }
    LARGE_INTEGER due;
    int64_t units = ns / 100 + (ns % 100 != 0);   // 100 ns units, rounded up
    due.QuadPart = -(units > 0 ? units : 1);
    if (!SetWaitableTimer(t->timer, &due, 0, NULL, NULL, FALSE))
        rt_fatal("kernel: SetWaitableTimer failed (error %lu)", GetLastError());
    WaitForSingleObject(t->timer, INFINITE);
}

// ---- synchronization objects
static int sig_obj(uint32_t h, int *bad) {   // 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) {
        *bad = 1;
        return 0;
    }
    switch (g_h[i].type) {
    case O_THREAD: return ((RtThread *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((Event *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}

int kern_thread_state(uint32_t h) {
    lock();
    RtThread *t = h_peek(h, O_THREAD);
    int r = !t ? -1 : t->exited ? KERN_THREAD_EXITED : t->wkind;
    unlock();
    return r;
}

// CloseHandle: an event goes with its last end; a thread object once it has also exited.
static uint32_t handle_close(uint32_t h) {
    unsigned i = h - 0x1000;
    int ok = 0, kind = 0;
    void *ipc = NULL;
    lock();
    if (i > 0 && i < MAX_H) {
        int type = g_h[i].type;
        void *o = g_h[i].obj;
        if (type == O_EVENT_R || type == O_EVENT_W) {
            Event *e = o;
            if (--e->refs == 0) free(e);
            ok = 1;
        } else if (type == O_SESSION || type == O_TMEM) {
            kind = type;
            ipc = o;
            ok = 1;
        } else if (type == O_THREAD) {
            RtThread *t = o;
            t->closed = 1;
            if (t->exited) thr_free(t);
            ok = 1;
        }
        if (ok) {
            g_h[i].type = O_FREE;
            g_h[i].obj = NULL;
        }
    }
    unlock();
    if (ipc) ipc_handle_closed(kind, ipc);
    return ok ? R_OK : R_BAD_HANDLE;
}

void kern_handle_close(uint32_t h) {
    if (handle_close(h)) rt_fatal("kern_handle_close(%#x): bad handle", h);
}

// Hooks of the probe's condvar negative control.
volatile int kern_test_cv_old_order;
void (*volatile kern_test_cv_hook)(void);

static void cv_hook(void) {
    void (*h)(void) = kern_test_cv_hook;
    if (h) h();
}

int kern_svc(Cpu *c, uint32_t imm) {
    uint64_t *x = c->x;
    switch (imm) {
    case 0x08: {   // CreateThread(-> w1 handle; x1 entry, x2 arg, x3 stack top, w4 priority, w5 core)
        int prio = (int)x[4], core = (int)x[5];
        if (!x[1] || !x[3] || prio < 28 || prio > 59 || core < -3 || core > 2) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        RtThread *t = thr_new(prio, core >= 0 ? core : MAIN_CORE, core >= 0 ? 1ull << core : 7);
        t->entry = x[1];
        t->arg = x[2];
        t->stack_top = x[3] & ~15ull;
        lock();
        thr_link(t);
        unlock();
        HANDLE h = CreateThread(NULL, HOST_STACK, thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!h) rt_fatal("kernel: CreateThread failed (error %lu)", GetLastError());
        CloseHandle(h);
        if (rt_trace & RT_TRACE_SVC) {
            char d[128];
            rt_log("CreateThread %#x entry %s stack %#llx prio %d core %d", t->handle,
                   ldr_describe(t->entry, d, sizeof d), (unsigned long long)t->stack_top, prio, core);
        }
        x[0] = R_OK;
        x[1] = t->handle;
        return 1;
    }
    case 0x09: {   // StartThread(w0)
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) {
            t->started = 1;
            WakeConditionVariable(&t->cv);
        }
        unlock();
        x[0] = t ? R_OK : R_BAD_HANDLE;
        return 1;
    }
    case 0x0A: {   // ExitThread
        RtThread *t = cur(imm);
        if (t->fiber) {   // on a guest fiber: unwind from the thread's own context (kern_fiber_switch resumes it)
            t->exit_pending = 1;
            t->leaving = t->fiber;
            t->fiber = NULL;
            t->cur = t->cpu;
            SwitchToFiber(t->base_fiber);
            rt_fatal("kernel: ExitThread on a fiber came back");
        }
        exit_now(t);
    }
    case 0x0B: {   // SleepThread(x0 ns); 0, -1, -2 yield
        int64_t ns = (int64_t)x[0];
        RtThread *t = cur(imm);
        if (ns > 0) {
            lock();
            core_put(t);
            acct(t, S_SLEEP);
            t->wobj = (uint64_t)ns;
            t->nblk++;
            unlock();
            sleep_ns(t, ns);
            lock();
            core_take(t);
            unlock();
            return 1;
        }
        // Yield: hand the core to a waiting thread of equal or better priority.
        lock();
        int give = 0;
        for (RtThread *w = g_core_wait; w && !give; w = w->cnext)
            give = t->cur_core >= 0 && core_allowed(w, t->cur_core) && w->prio <= t->prio;
        if (give) {
            core_put(t);
            core_take(t);
        }
        unlock();
        if (!give) SwitchToThread();
        return 1;
    }
    case 0x0C: {   // GetThreadPriority(w1 handle -> w1)
        lock();
        RtThread *t = h_get((uint32_t)x[1], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) x[1] = (uint64_t)t->prio;
        unlock();
        return 1;
    }
    case 0x0D: {   // SetThreadPriority(w0 handle, w1 priority)
        if ((uint32_t)x[1] < 28 || (uint32_t)x[1] > 59) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) t->prio = (int)x[1];
        x[0] = t ? R_OK : R_BAD_HANDLE;
        unlock();
        return 1;
    }
    case 0x0E: {   // GetThreadCoreMask(w2 handle -> w1 core, x2 mask)
        lock();
        RtThread *t = h_get((uint32_t)x[2], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) {
            x[1] = (uint64_t)(uint32_t)t->core;
            x[2] = t->mask;
        }
        unlock();
        return 1;
    }
    case 0x0F: {   // SetThreadCoreMask(w0 handle, w1 ideal core, x2 mask): -1 process default, -2/-3 keep the ideal core
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) {
            int core = (int)x[1];
            if (core >= 0) t->core = core;
            else if (core == -1) t->core = MAIN_CORE;
            if (x[2]) t->mask = x[2];
            if (t == tcur && t->cur_core >= 0 && !core_allowed(t, t->cur_core)) {   // move off a forbidden core
                core_put(t);
                core_take(t);
            }
        }
        x[0] = t ? R_OK : R_BAD_HANDLE;
        unlock();
        return 1;
    }
    case 0x10: {   // GetCurrentProcessorNumber: the virtual core held
        RtThread *t = cur(imm);
        x[0] = (uint64_t)(uint32_t)(t->cur_core >= 0 ? t->cur_core : t->core);
        return 1;
    }
    case 0x36: {   // SynchronizePreemptionState: the SDK's spin locks call it after clearing TLR+0x100 (disable_count)
                   // when TLR+0x102 (interrupt_flag) is set; rt_svc's checkpoint has already honoured a pending preempt
        RtThread *t = cur(imm);
        volatile uint16_t *flag = (volatile uint16_t *)(t->tlr + 0x102);
        __atomic_fetch_add(&g_sync36, 1, __ATOMIC_RELAXED);
        if (*flag) __atomic_fetch_add(&g_sync36_flag, 1, __ATOMIC_RELAXED);
        *flag = 0;
        x[0] = R_OK;
        return 1;
    }
    case 0x25: {   // GetThreadId(w1 handle -> x1)
        lock();
        RtThread *t = h_get((uint32_t)x[1], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) x[1] = t->id;
        unlock();
        return 1;
    }

    case 0x1A: {   // ArbitrateLock(w0 owner handle, x1 address, w2 tag)
        RtThread *t = cur(imm);
        uint64_t addr = x[1];
        uint32_t r = R_OK;
        lock();
        if (ld32(addr) == ((uint32_t)x[0] | WAITERS_BIT)) {
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
            t->wobj = (uint32_t)x[0];
            r = block(t, W_LOCK, -1);
        }
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x1B:   // ArbitrateUnlock(x0 address)
        lock();
        lock_handoff(x[0]);
        unlock();
        x[0] = R_OK;
        return 1;
    case 0x1C: {   // WaitProcessWideKeyAtomic(x0 lock, x1 key, w2 tag, x3 timeout)
        // The key is set before the lock is released. The SDK's Signal reads the key in user mode and skips svc 0x1D
        // when it is 0, and user mode takes the lock without G: releasing first lets another thread take the lock,
        // change the condition, read key 0 and wake nobody, after which this thread waits forever (MNG threads.c
        // 439-446). Linking into the wait list happens in block, still holding G, so svc 0x1D cannot come between.
        RtThread *t = cur(imm);
        uint64_t addr = x[0], key = x[1];
        lock();
        if (!kern_test_cv_old_order) {
            st32(key, 1);
            t->wkey = key;
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
            lock_handoff(addr);
            cv_hook();
        } else {   // the probe's negative control: MNG's pre-fix order
            lock_handoff(addr);
            cv_hook();
            st32(key, 1);
            t->wkey = key;
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
        }
        t->wobj = key;
        uint32_t r = block(t, W_CONDVAR, (int64_t)x[3]);   // on timeout user mode takes the lock itself
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x1D: {   // SignalProcessWideKey(x0 key, w1 count; <= 0 all)
        uint64_t key = x[0];
        int cnt = (int)x[1], n = 0;
        lock();
        for (RtThread *w; (cnt <= 0 || n < cnt) && (w = pick(W_CONDVAR, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);
        int left = 0;
        for (RtThread *w = g_wait; w && !left; w = w->next) left = w->wkind == W_CONDVAR && w->wkey == key;
        if (!left) st32(key, 0);
        unlock();
        x[0] = R_OK;
        return 1;
    }

    case 0x34: {   // WaitForAddress(x0 address, w1 type, w2 value, x3 timeout): 0 if less, 1 decrement if less, 2 if equal
        RtThread *t = cur(imm);
        uint64_t addr = x[0];
        int type = (int)x[1];
        int32_t val = (int32_t)x[2];
        if (type < 0 || type > 2) {
            x[0] = R_INVALID_ENUM;
            return 1;
        }
        lock();
        uint32_t r, v = ld32(addr);
        int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1)
            while (!cas32(addr, &v, v - 1))
                if (!((int32_t)v < val)) {
                    ok = 0;
                    break;
                }
        if (!ok) r = R_BAD_STATE;
        else {
            t->waddr = addr;
            t->wobj = addr;
            r = block(t, W_ADDRESS, (int64_t)x[3]);
        }
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x35: {   // SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal,
                   // 2 if equal modify by the number of waiters
        uint64_t addr = x[0];
        int type = (int)x[1], cnt = (int)x[3];
        int32_t val = (int32_t)x[2];
        if (type < 0 || type > 2) {
            x[0] = R_INVALID_ENUM;
            return 1;
        }
        lock();
        int nw = 0;
        for (RtThread *w = g_wait; w; w = w->next) nw += w->wkind == W_ADDRESS && w->waddr == addr;
        uint32_t r = R_OK;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                if (cnt <= 0) nv = nw > 0 ? val - 1 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r)
            for (int n = 0; cnt <= 0 || n < cnt; n++) {
                RtThread *w = pick(W_ADDRESS, addr);
                if (!w) break;
                wake(w, 0);
            }
        unlock();
        x[0] = r;
        return 1;
    }

    case 0x45: {   // CreateEvent(-> w1 writable, w2 readable)
        Event *e = calloc(1, sizeof *e);
        if (!e) rt_fatal("kernel: event allocation");
        e->refs = 2;
        lock();
        uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e);
        unlock();
        x[0] = R_OK;
        x[1] = hw;
        x[2] = hr;
        return 1;
    }
    case 0x11: case 0x12: case 0x17: {   // SignalEvent(writable), ClearEvent(either end), ResetSignal(readable)
        uint32_t h = (uint32_t)x[0];
        uint32_t r = R_OK;
        lock();
        Event *e = imm == 0x11 ? h_peek(h, O_EVENT_W) : imm == 0x17 ? h_peek(h, O_EVENT_R)
                 : h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R);
        if (!e) {
            r = imm == 0x17 && h_peek(h, O_THREAD) ? R_BAD_STATE : R_BAD_HANDLE;
            if (r == R_BAD_HANDLE) rt_log("kernel: event svc %#x: invalid handle %#x", imm, h);
        } else if (imm == 0x11) {
            e->signaled = 1;
            wake_sync_waiters();
        } else if (imm == 0x17 && !e->signaled) {
            r = R_BAD_STATE;
        } else {
            e->signaled = 0;
        }
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x18: {   // WaitSynchronization(-> w1 index; x1 handles, w2 count, x3 timeout)
        RtThread *t = cur(imm);
        const uint32_t *hs = (const uint32_t *)(uintptr_t)x[1];
        int n = (int)x[2];
        int64_t ns = (int64_t)x[3];
        if (n < 0 || n > 64 || (!hs && n)) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        uint64_t start = kern_now_ns();
        uint32_t r;
        int idx = -1;
        lock();
        g_sync_calls++;
        for (;;) {
            if (t->cancel) {
                t->cancel = 0;
                r = R_CANCELLED;
                break;
            }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++)
                if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) {
                r = R_BAD_HANDLE;
                break;
            }
            if (idx >= 0) {
                r = R_OK;
                break;
            }
            int64_t left = ns;
            if (ns > 0) {
                uint64_t spent = kern_now_ns() - start;
                if (spent >= (uint64_t)ns) {
                    r = R_TIMEOUT;
                    break;
                }
                left = ns - (int64_t)spent;
            }
            t->wobj = n ? hs[0] | (uint64_t)n << 32 : 0;
            g_sync_blocks++;
            if (block(t, W_SYNC, left) == R_TIMEOUT) {
                r = R_TIMEOUT;
                break;
            }
        }
        unlock();
        x[0] = r;
        x[1] = idx >= 0 ? (uint64_t)idx : 0;
        return 1;
    }
    case 0x19: {   // CancelSynchronization(w0 thread)
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) {
            t->cancel = 1;
            if (t->wkind == W_SYNC) wake(t, (uint32_t)-1);
        }
        unlock();
        x[0] = t ? R_OK : R_BAD_HANDLE;
        return 1;
    }
    case 0x16:   // CloseHandle(w0)
        x[0] = handle_close((uint32_t)x[0]);
        return 1;
    }
    return 0;
}

// ---- handles and events for host services (ipc.c and the service overrides)
uint32_t kern_handle_new(int kind, void *obj) {
    if (kind != KERN_SESSION && kind != KERN_TMEM) rt_fatal("kern_handle_new: kind %d", kind);
    lock();
    uint32_t h = h_new(kind, obj);
    unlock();
    return h;
}

void *kern_handle_get(uint32_t h, int kind) {
    lock();
    void *o = h_peek(h, kind);
    unlock();
    return o;
}

uint32_t kern_event_new(void) {
    Event *e = calloc(1, sizeof *e);
    if (!e) rt_fatal("kernel: event allocation");
    e->refs = 1;
    lock();
    uint32_t hw = h_new(O_EVENT_W, e);
    unlock();
    return hw;
}

// Copy-handle transfer of the readable end: each call gives the client its own handle to the same event.
uint32_t kern_event_readable(uint32_t hw) {
    lock();
    Event *e = h_peek(hw, O_EVENT_W);
    if (!e) rt_fatal("kern_event_readable: %#x is not a writable event handle", hw);
    e->refs++;
    uint32_t hr = h_new(O_EVENT_R, e);
    unlock();
    return hr;
}

void kern_event_signal(uint32_t hw) {
    lock();
    Event *e = h_peek(hw, O_EVENT_W);
    if (!e) rt_fatal("kern_event_signal: %#x is not a writable event handle", hw);
    e->signaled = 1;
    wake_sync_waiters();
    unlock();
}

// System events of host services: the SDK's own nn::os::CreateSystemEvent creates the kernel event (SVC 0x45) on a
// guest thread; the writable handle is kept, so any thread (host audio tick included) can signal it later.
static struct { uint64_t ev; uint32_t wh; } g_se[512];
static int g_nse;
static SRWLOCK g_se_lock = SRWLOCK_INIT;

static uint32_t se_find(uint64_t ev) {
    AcquireSRWLockShared(&g_se_lock);
    uint32_t wh = 0;
    for (int i = 0; i < g_nse && !wh; i++)
        if (g_se[i].ev == ev) wh = g_se[i].wh;
    ReleaseSRWLockShared(&g_se_lock);
    return wh;
}

void kern_bind_system_event(Cpu *c, uint64_t ev, int autoclear) {
    if (se_find(ev)) return;
    if (!tcur || !c) rt_fatal("kern_bind_system_event(%#llx) on a host thread", (unsigned long long)ev);
    static uint64_t create, getw;
    if (!create) {
        create = ldr_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
        getw = ldr_symbol("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE");
        if (!create || !getw) rt_fatal("kern_bind_system_event: the SDK's system event functions are not loaded");
    }
    uint32_t r = (uint32_t)rt_call(c, create, 3, ev, (uint64_t)(autoclear ? 1 : 0), (uint64_t)1);
    if (r) rt_fatal("kern_bind_system_event: CreateSystemEvent(%#llx) failed %#x", (unsigned long long)ev, r);
    uint32_t wh = (uint32_t)rt_call(c, getw, 1, ev);
    AcquireSRWLockExclusive(&g_se_lock);
    if (g_nse == (int)(sizeof g_se / sizeof *g_se)) rt_fatal("kern_bind_system_event: table full");
    g_se[g_nse].ev = ev;
    g_se[g_nse++].wh = wh;
    ReleaseSRWLockExclusive(&g_se_lock);
}

// An event not bound yet is bound on the spot with manual clear, which needs a guest thread.
void kern_signal_system_event(uint64_t ev) {
    uint32_t wh = se_find(ev);
    if (!wh) {
        kern_bind_system_event(kern_cpu(), ev, 0);
        wh = se_find(ev);
    }
    kern_event_signal(wh);
}

// ---- fibers
// The SDK switches fibers by swapping register context, which breaks "RET = C return". Each guest fiber gets a host
// fiber (own 256 MiB reserved host stack) and its own Cpu, so a suspended fiber's registers and host frames stay
// untouched while others run, and a fiber may resume on another thread. A switch carries the thread state (TLR, TLS
// pointer, FPCR/FPSR) over to the target's Cpu and clears the exclusive monitor.
enum { F_NEW, F_STARTED, F_DONE };
struct Fiber {
    uint64_t addr, func, arg, stack, size;
    Cpu *cpu;
    void *host;
    int state;
    volatile int running;
};
static Fiber *g_fibers[FIBER_MAX];
static SRWLOCK g_fiber_lock = SRWLOCK_INIT;

static Fiber *fiber_find(uint64_t addr) {
    Fiber *f = NULL;
    AcquireSRWLockShared(&g_fiber_lock);
    for (int i = 0; i < FIBER_MAX && !f; i++)
        if (g_fibers[i] && g_fibers[i]->addr == addr) f = g_fibers[i];
    ReleaseSRWLockShared(&g_fiber_lock);
    return f;
}

static void fiber_switch_to(RtThread *t, const Cpu *from, Fiber *to);

// After a host fiber switch: release the context left behind, and finish an ExitThread issued on a fiber.
static void fiber_resumed(void) {
    RtThread *t = tcur;
    if (t->leaving) {
        __atomic_store_n(&t->leaving->running, 0, __ATOMIC_RELEASE);
        t->leaving = NULL;
    }
    if (t->exit_pending && !t->fiber) exit_now(t);
}

static void WINAPI fiber_main(void *p) {
    Fiber *f = p;
    fiber_resumed();
    Cpu *c = f->cpu;
    tcur->host_depth = 0;   // a new fiber starts at the top level; each context restores its own depth on resume
    uint64_t next = rt_call(c, f->func, 1, f->arg);
    f->state = F_DONE;
    Fiber *n = NULL;
    if (next && !(n = fiber_find(next)))
        rt_fatal("fiber %#llx returned %#llx, which is not an initialized fiber", (unsigned long long)f->addr,
                 (unsigned long long)next);
    fiber_switch_to(tcur, c, n);   // NULL: back to the thread's own context
    rt_fatal("fiber %#llx resumed after it finished", (unsigned long long)f->addr);
}

static void fiber_switch_to(RtThread *t, const Cpu *from, Fiber *to) {
    if (!t->base_fiber) {
        t->base_fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
        if (!t->base_fiber) rt_fatal("kernel: ConvertThreadToFiberEx failed (error %lu)", GetLastError());
    }
    Cpu *dst = to ? to->cpu : t->cpu;
    if (to) {
        if (to->state == F_DONE) rt_fatal("SwitchToFiber(%#llx): the fiber has finished", (unsigned long long)to->addr);
        int idle = 0;
        if (!__atomic_compare_exchange_n(&to->running, &idle, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            rt_fatal("SwitchToFiber(%#llx): the fiber is running", (unsigned long long)to->addr);
        if (to->state == F_NEW) {
            dst->sp = (to->stack + to->size) & ~15ull;
            to->state = F_STARTED;
        }
    }
    dst->tpidr = from->tpidr;
    dst->tpidrro = from->tpidrro;
    dst->fpcr = from->fpcr;
    dst->fpcr_host = 0;   // the target host fiber resumes with its own MXCSR; simd_fast resyncs it
    dst->fpsr = from->fpsr;
    dst->excl_addr = ~0ull;
    dst->thread = t;
    t->leaving = t->fiber;
    t->fiber = to;
    t->cur = dst;
    int depth = t->host_depth;
    SwitchToFiber(to ? to->host : t->base_fiber);
    t->host_depth = depth;
    fiber_resumed();
}

// InitializeFiber(x0 FiberType*, x1 function, x2 argument, x3 stack, x4 stack size, w5 flags)
void kern_fiber_initialize(Cpu *c) {
    uint64_t addr = c->x[0];
    Fiber *f = fiber_find(addr);
    if (f && f->running) rt_fatal("InitializeFiber(%#llx): the fiber is running", (unsigned long long)addr);
    if (!f) {
        f = calloc(1, sizeof *f);
        Cpu *fc = _aligned_malloc(sizeof(Cpu), 64);
        if (!f || !fc) rt_fatal("kernel: fiber allocation");
        f->cpu = fc;
        AcquireSRWLockExclusive(&g_fiber_lock);
        int i = 0;
        while (i < FIBER_MAX && g_fibers[i]) i++;
        if (i == FIBER_MAX) rt_fatal("kernel: more than %d fibers", FIBER_MAX);
        g_fibers[i] = f;
        ReleaseSRWLockExclusive(&g_fiber_lock);
    } else if (f->host) {
        DeleteFiber(f->host);
    }
    memset(f->cpu, 0, sizeof *f->cpu);
    f->cpu->excl_addr = ~0ull;
    f->addr = addr;
    f->func = c->x[1];
    f->arg = c->x[2];
    f->stack = c->x[3];
    f->size = c->x[4];
    f->state = F_NEW;
    f->running = 0;
    f->host = CreateFiberEx(0x10000, HOST_STACK, FIBER_FLAG_FLOAT_SWITCH, fiber_main, f);
    if (!f->host) rt_fatal("kernel: CreateFiberEx failed (error %lu)", GetLastError());
}

// FinalizeFiber(x0 FiberType*)
void kern_fiber_finalize(Cpu *c) {
    uint64_t addr = c->x[0];
    AcquireSRWLockExclusive(&g_fiber_lock);
    Fiber *f = NULL;
    for (int i = 0; i < FIBER_MAX && !f; i++)
        if (g_fibers[i] && g_fibers[i]->addr == addr) {
            f = g_fibers[i];
            g_fibers[i] = NULL;
        }
    ReleaseSRWLockExclusive(&g_fiber_lock);
    if (!f) rt_fatal("FinalizeFiber(%#llx): not an initialized fiber", (unsigned long long)addr);
    if (f->running) rt_fatal("FinalizeFiber(%#llx): the fiber is running", (unsigned long long)addr);
    DeleteFiber(f->host);
    _aligned_free(f->cpu);
    free(f);
}

// SwitchToFiber(x0 FiberType*)
void kern_fiber_switch(Cpu *c) {
    RtThread *t = tcur;
    if (!t) rt_fatal("SwitchToFiber on a host thread");
    Fiber *to = fiber_find(c->x[0]);
    if (!to) rt_fatal("SwitchToFiber(%#llx): not an initialized fiber", (unsigned long long)c->x[0]);
    if (to == t->fiber) return;
    fiber_switch_to(t, c, to);
}

// GetCurrentFiber() -> x0, NULL on the thread's own context
void kern_fiber_current(Cpu *c) {
    RtThread *t = tcur;
    c->x[0] = t && t->fiber ? t->fiber->addr : 0;
}

HOST_OVERRIDE(KERN_SYM_INITIALIZE_FIBER, kern_fiber_initialize);
HOST_OVERRIDE(KERN_SYM_FINALIZE_FIBER, kern_fiber_finalize);
HOST_OVERRIDE(KERN_SYM_SWITCH_TO_FIBER, kern_fiber_switch);
HOST_OVERRIDE(KERN_SYM_GET_CURRENT_FIBER, kern_fiber_current);
