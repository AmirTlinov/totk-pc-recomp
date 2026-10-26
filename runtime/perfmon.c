// Performance overlay numbers (runtime/perfmon.h). Frame times come from the game's presents (perf_present, only while
// the overlay is on); the per-frame waits from the perf counters prof_win.c keeps. The machine's numbers are sampled
// twice a second on a thread of their own (some calls take milliseconds), which runs only while the overlay is full.
// The OS calls sit behind platform.h (plat_cpu_load, plat_mem_info, plat_gpu_load); NVML (NVIDIA's driver library,
// for temperature, power and clock) is loaded at run time and is missing on other machines: its numbers stay -1.
//
// Adapted from SavageWindRecomp's runtime/perfmon.c (https://github.com/Revivedx/SavageWindRecomp @
// 575ad88e02f9a89a9a885d90039d48af33c982f4, MIT, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors,
// Copyright (c) 2026 Peiikai): the frame ring and its one-second summary, the NVML calls, the sampler loop. The
// Windows, Linux and macOS sampling moved behind platform.h, GPU load and video memory come from the system's counters
// for every vendor, and the sampler stops when the overlay leaves full.
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "perfmon.h"
#include "platform/platform.h"
#include "sysinfo.h"

static PlatMutex g_lock = PLAT_MUTEX_INIT;   // everything below
static PerfStats g_stats;                    // the machine numbers the sampler wrote; the rest is made per snapshot

// ---- frames
static float g_ring[PERF_HISTORY];
static uint64_t g_ring_at[PERF_HISTORY];   // when each frame ended
static int g_ring_n, g_ring_pos;

// The per-frame counters, summed since t0 (the overlay's lines are averages over half a second).
static struct {
    uint64_t t0, frames, gpu0;
    double replay, pipe, xlate, tex;
    int64_t up0, up;
} g_win;

void perfmon_frame(uint64_t now, uint64_t interval_ns, const uint64_t *wait_ns, uint64_t replay_ns,
                   int64_t tex_uploads) {
    plat_mutex_lock(&g_lock);
    if (interval_ns) {
        g_ring[g_ring_pos] = (float)(interval_ns / 1e6);
        g_ring_at[g_ring_pos] = now;
        g_ring_pos = (g_ring_pos + 1) % PERF_HISTORY;
        if (g_ring_n < PERF_HISTORY) g_ring_n++;
    }
    g_win.frames++;
    g_win.replay += replay_ns / 1e6;
    g_win.pipe += wait_ns[PERF_PIPE] / 1e6;
    g_win.xlate += wait_ns[PERF_XLATE] / 1e6;
    g_win.tex += wait_ns[PERF_TEX] / 1e6;
    g_win.up = tex_uploads;
    plat_mutex_unlock(&g_lock);
}

static void frames_summary(PerfStats *s, uint64_t now) {   // (lock held)
    double sum = 0, mx = 0;
    int n = 0;
    for (int i = 0; i < g_ring_n; i++) {
        int k = (g_ring_pos - 1 - i + PERF_HISTORY) % PERF_HISTORY;
        if (now - g_ring_at[k] > 1000000000ull) break;   // the last second
        sum += g_ring[k];
        if (g_ring[k] > mx) mx = g_ring[k];
        n++;
    }
    s->fps = n && sum > 0 ? (float)(n * 1000.0 / sum) : 0;
    s->frame_ms_avg = n ? (float)(sum / n) : 0;
    s->frame_ms_max = (float)mx;
    for (int i = 0; i < PERF_HISTORY; i++) {   // oldest first; frames not seen yet read 0
        int age = PERF_HISTORY - 1 - i;
        s->frame_ms[i] = age < g_ring_n ? g_ring[(g_ring_pos - 1 - age + 2 * PERF_HISTORY) % PERF_HISTORY] : 0;
    }
}

// Every half second: the window's sums as per-frame averages into g_stats, and a new window. (lock held)
static void counters_summary(uint64_t now) {
    if (now - g_win.t0 < 500000000ull) return;
    uint64_t gpu = perf_gpu_busy_total();
    if (g_win.t0 && g_win.frames) {
        double n = (double)g_win.frames;
        g_stats.replay_ms = (float)(g_win.replay / n);
        g_stats.pipe_wait_ms = (float)(g_win.pipe / n);
        g_stats.xlate_ms = (float)(g_win.xlate / n);
        g_stats.tex_ms = (float)(g_win.tex / n);
        g_stats.tex_uploads = (float)((g_win.up - g_win.up0) / n);
        // no GPU time over a window of frames: no timestamps on this device (or not yet set up)
        g_stats.gpu_ms = gpu > g_win.gpu0 ? (float)((gpu - g_win.gpu0) / 1e6 / n) : -1;
    }
    g_win.t0 = now;
    g_win.frames = 0;
    g_win.gpu0 = gpu;
    g_win.replay = g_win.pipe = g_win.xlate = g_win.tex = 0;
    g_win.up0 = g_win.up;
}

// ---- NVML (NVIDIA): loaded once on the sampler thread, kept for the process
typedef struct { unsigned gpu, memory; } NvmlUtil;
typedef struct { unsigned long long total, free, used; } NvmlMem;
static struct {
    int tried, ok;
    void *dev;
    int (*util)(void *, NvmlUtil *);
    int (*temp)(void *, int, unsigned *);
    int (*mem)(void *, NvmlMem *);
    int (*power)(void *, unsigned *);
    int (*clock)(void *, int, unsigned *);
} nv;

static char g_gpu_name[96];
static uint8_t g_gpu_luid[8];
static int g_gpu_luid_ok;
static float g_vram_total_mb = -1;

void perfmon_set_gpu(const char *name, const uint8_t *luid, float vram_total_mb) {
    plat_mutex_lock(&g_lock);
    snprintf(g_gpu_name, sizeof g_gpu_name, "%s", name ? name : "");
    g_gpu_luid_ok = luid != NULL;
    if (luid) memcpy(g_gpu_luid, luid, sizeof g_gpu_luid);
    g_vram_total_mb = vram_total_mb > 0 ? vram_total_mb : -1;
    plat_mutex_unlock(&g_lock);
}

static void nvml_open(const char *hint) {
    nv.tried = 1;
#ifdef _WIN32
    static const char *const names[] = {"nvml.dll", "C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll"};
#else
    static const char *const names[] = {"libnvidia-ml.so.1"};
#endif
    void *lib = NULL;
    for (size_t i = 0; !lib && i < sizeof names / sizeof *names; i++) lib = plat_lib_open(names[i]);
    if (!lib) return;
    int (*init)(void) = (int (*)(void))plat_lib_sym(lib, "nvmlInit_v2");
    int (*count)(unsigned *) = (int (*)(unsigned *))plat_lib_sym(lib, "nvmlDeviceGetCount_v2");
    int (*handle)(unsigned, void **) = (int (*)(unsigned, void **))plat_lib_sym(lib, "nvmlDeviceGetHandleByIndex_v2");
    int (*name)(void *, char *, unsigned) = (int (*)(void *, char *, unsigned))plat_lib_sym(lib, "nvmlDeviceGetName");
    nv.util = (int (*)(void *, NvmlUtil *))plat_lib_sym(lib, "nvmlDeviceGetUtilizationRates");
    nv.temp = (int (*)(void *, int, unsigned *))plat_lib_sym(lib, "nvmlDeviceGetTemperature");
    nv.mem = (int (*)(void *, NvmlMem *))plat_lib_sym(lib, "nvmlDeviceGetMemoryInfo");
    nv.power = (int (*)(void *, unsigned *))plat_lib_sym(lib, "nvmlDeviceGetPowerUsage");
    nv.clock = (int (*)(void *, int, unsigned *))plat_lib_sym(lib, "nvmlDeviceGetClockInfo");
    if (!init || !count || !handle || init() != 0) return;
    unsigned n = 0;
    if (count(&n) != 0 || !n) return;
    // the device Vulkan renders on (by name; an NVIDIA name only matches an NVIDIA GPU), else none: on a machine
    // that renders on another vendor's GPU, NVML's numbers would belong to the wrong card
    for (unsigned i = 0; i < n && !nv.dev; i++) {
        void *d = NULL;
        char nm[96] = "";
        if (handle(i, &d) != 0 || !name || name(d, nm, sizeof nm) != 0 || !nm[0]) continue;
        if (hint[0] && (strstr(hint, nm) || strstr(nm, hint))) nv.dev = d;
    }
    nv.ok = nv.dev != NULL;
    rt_log("perfmon: NVML %s", nv.ok ? "found the rendering GPU" : "has no device matching the rendering GPU");
}

static void nvml_sample(PerfStats *s) {
    if (!nv.ok) return;
    NvmlUtil u;
    NvmlMem m;
    unsigned v;
    if (s->gpu_pct < 0 && nv.util && nv.util(nv.dev, &u) == 0) s->gpu_pct = (float)u.gpu;   // the system's counters first
    if (nv.mem && nv.mem(nv.dev, &m) == 0) {
        if (s->vram_used_mb < 0) s->vram_used_mb = (float)(m.used >> 20);
        s->vram_total_mb = (float)(m.total >> 20);
    }
    if (nv.temp && nv.temp(nv.dev, 0 /* NVML_TEMPERATURE_GPU */, &v) == 0) s->gpu_temp_c = (float)v;
    if (nv.power && nv.power(nv.dev, &v) == 0) s->gpu_power_w = v / 1000.0f;
    if (nv.clock && nv.clock(nv.dev, 0 /* NVML_CLOCK_GRAPHICS */, &v) == 0) s->gpu_clock_mhz = (float)v;
}

// ---- the machine sampler: runs while want is set, twice a second
static struct {
    int want, alive;
    PlatCond cv;
} g_smp = {0, 0, PLAT_COND_INIT};

static void machine_clear(PerfStats *s) {
    s->cpu_total_pct = s->cpu_game_pct = s->cpu_temp_c = -1;
    s->ram_used_mb = s->ram_total_mb = s->ram_game_mb = -1;
    s->gpu_pct = s->gpu_temp_c = s->gpu_power_w = s->gpu_clock_mhz = s->vram_used_mb = s->vram_total_mb = -1;
}

static void machine_copy(PerfStats *d, const PerfStats *s) {
    memcpy(d->cpu_name, s->cpu_name, sizeof d->cpu_name);
    memcpy(d->gpu_name, s->gpu_name, sizeof d->gpu_name);
    d->cpu_total_pct = s->cpu_total_pct, d->cpu_game_pct = s->cpu_game_pct, d->cpu_temp_c = s->cpu_temp_c;
    d->ram_used_mb = s->ram_used_mb, d->ram_total_mb = s->ram_total_mb, d->ram_game_mb = s->ram_game_mb;
    d->gpu_pct = s->gpu_pct, d->gpu_temp_c = s->gpu_temp_c, d->gpu_power_w = s->gpu_power_w;
    d->gpu_clock_mhz = s->gpu_clock_mhz, d->vram_used_mb = s->vram_used_mb, d->vram_total_mb = s->vram_total_mb;
}

static void sampler(void *arg) {
    (void)arg;
    PerfStats s;
    memset(&s, 0, sizeof s);
    plat_mutex_lock(&g_lock);
    uint8_t luid[8];
    int have_luid = g_gpu_luid_ok;
    memcpy(luid, g_gpu_luid, sizeof luid);
    snprintf(s.gpu_name, sizeof s.gpu_name, "%s", g_gpu_name);
    float vram_total = g_vram_total_mb;
    plat_mutex_unlock(&g_lock);
    sysinfo_cpu_name(s.cpu_name, sizeof s.cpu_name);
    if (!nv.tried) nvml_open(s.gpu_name);
    PlatLoad *l = plat_load_open(have_luid ? luid : NULL);
    plat_mutex_lock(&g_lock);
    while (g_smp.want) {
        plat_mutex_unlock(&g_lock);
        machine_clear(&s);
        plat_cpu_load(l, &s.cpu_total_pct, &s.cpu_game_pct, &s.cpu_temp_c);
        plat_mem_info(&s.ram_used_mb, &s.ram_total_mb, &s.ram_game_mb);
        plat_gpu_load(l, &s.gpu_pct, &s.vram_used_mb);
        s.vram_total_mb = vram_total;
        nvml_sample(&s);
        plat_mutex_lock(&g_lock);
        machine_copy(&g_stats, &s);
        if (g_smp.want) plat_cond_wait_ns(&g_smp.cv, &g_lock, 500000000ull);
    }
    g_smp.alive = 0;   // in the same locked section as the last want check: a new perfmon_mode(2) starts a new thread
    plat_mutex_unlock(&g_lock);
    plat_load_close(l);
}

void perfmon_mode(int mode) {
    perf_set_overlay(mode);
    plat_mutex_lock(&g_lock);
    if (mode == 2) {
        g_smp.want = 1;
        if (!g_smp.alive) {
            machine_clear(&g_stats);   // "-" until the first sample, not the numbers of the last time
            PlatThread *t = plat_thread_new(sampler, NULL, "perfmon");
            g_smp.alive = t != NULL;
            if (t) plat_thread_detach(t);
            else rt_log("perfmon: cannot start the sampling thread; the machine numbers stay empty");
        }
    } else {
        g_smp.want = 0;
        plat_cond_signal(&g_smp.cv);
    }
    if (!mode) {   // the next start begins a new graph and a new counter window
        g_ring_n = g_ring_pos = 0;
        memset(&g_win, 0, sizeof g_win);
    }
    plat_mutex_unlock(&g_lock);
}

void perfmon_snapshot(PerfStats *out) {
    uint64_t now = plat_now_ns();
    plat_mutex_lock(&g_lock);
    frames_summary(&g_stats, now);
    counters_summary(now);
    *out = g_stats;
    plat_mutex_unlock(&g_lock);
}
