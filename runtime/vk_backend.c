// The Vulkan graphics back end: gfx_vulkan_backend wires the memory and
// texture layer (vk_memory.c, vk_texture.c), submission and fences (vk_sync.c) and the present. Every entry starts the
// device on first use.
//
// Present: the game texture is resolved on the calling thread and its blit is handed to a present thread, so
// nvnQueuePresentTexture returns at once (section 5). Queue order does the rest: the submissions that wrote the texture
// are already on gfx_queue, and every call that may write or free GPU state the blit reads (submit, finish, pool and
// texture teardown, texel writes) first waits until the queued blit was submitted (vk_present_drain).
//
// Back-edge pacing: the present thread submits the blit at once, then holds the host present (vkQueuePresentKHR) until
// the frame's display time from the pacer (nvn_api_obj.c), so present-to-present intervals stay even whatever the
// replay took. The blit is already on the queue, so the wait blocks no submission.
#include <windows.h>

#include "flight.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"

#define WEAK __attribute__((weak))

typedef struct { VkImage image; VkFormat format; int w, h, has_crop, crop[4]; uint64_t frame, at, queued; } PresentJob;

static SRWLOCK pl = SRWLOCK_INIT;
static CONDITION_VARIABLE pcv = CONDITION_VARIABLE_INIT;   // job queued, job taken, job submitted
static PresentJob job;
static int pending, busy, present_thread;

// Sleeps on a high-resolution timer to 1 ms before at, then yields until at: the timer alone wakes up to ~0.5 ms late.
static void wait_until(HANDLE timer, uint64_t at) {
    uint64_t now = kern_now_ns();
    if (at > now + 1500000 && timer) {
        LARGE_INTEGER d = {.QuadPart = -(LONGLONG)((at - now - 1000000) / 100)};
        if (SetWaitableTimer(timer, &d, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
    }
    while (kern_now_ns() < at) SwitchToThread();
}

static DWORD WINAPI present_main(void *arg) {
    (void)arg;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    uint64_t last = 0, last_queued = 0;
    AcquireSRWLockExclusive(&pl);
    for (;;) {
        while (!pending) SleepConditionVariableSRW(&pcv, &pl, INFINITE, 0);
        PresentJob j = job;
        pending = 0;
        busy = 1;
        WakeAllConditionVariable(&pcv);
        ReleaseSRWLockExclusive(&pl);
        if (flight_on) vk_thumbs_frame(j.frame);
        vk_present_blit(j.image, VK_IMAGE_LAYOUT_GENERAL, j.w, j.h, j.has_crop ? j.crop : NULL);
        vk_sync_submit(NULL, 0);   // a timeline value after the blit: deferred destruction waits for the blit too
        AcquireSRWLockExclusive(&pl);
        busy = 0;
        WakeAllConditionVariable(&pcv);
        ReleaseSRWLockExclusive(&pl);
        uint64_t t0 = kern_now_ns();
        wait_until(timer, j.at);
        uint64_t t1 = kern_now_ns();
        vk_present_flip();
        // TOTK_TRACE=present: one line per present: the host present's interval, the interval of the game's present
        // calls (when the host present went out before back-edge pacing), the wait and how far past its target it went
        if (rt_trace & RT_TRACE_PRESENT)
            rt_log("present: frame %llu interval %.3f ms, game %.3f ms, waited %.3f ms, %.3f ms past target",
                   (unsigned long long)j.frame, last ? (t1 - last) / 1e6 : 0.0,
                   last_queued ? (j.queued - last_queued) / 1e6 : 0.0, (t1 - t0) / 1e6,
                   j.at && t1 > j.at ? (t1 - j.at) / 1e6 : 0.0);
        last = t1;
        last_queued = j.queued;
        vk_pipeline_autosave();   // off the render thread, outside the present lock
        AcquireSRWLockExclusive(&pl);
    }
    return 0;
}

void vk_present_drain(void) {
    uint64_t p0 = perf_begin();
    AcquireSRWLockExclusive(&pl);
    while (pending || busy) SleepConditionVariableSRW(&pcv, &pl, INFINITE, 0);
    ReleaseSRWLockExclusive(&pl);
    perf_end(PERF_DRAIN, p0);
}

static void vk_be_present(const tex_t *t, const int crop[4], uint64_t at_ns) {
    static volatile long n_skipped;
    vk_sync_start();
    uint64_t frame = __atomic_add_fetch(&gfx_counters.frames, 1, __ATOMIC_RELAXED);
    perf_present(frame);   // before vk_exec_frame_log, which resets the frame's exec counts the flight recorder reads
    vk_exec_frame_log(t, frame);
    if (vk_dump_armed) vk_dump_present(frame);
    __atomic_store_n(&gfx_counters.validation_errors, (uint64_t)vk_validation_errors_total(), __ATOMIC_RELAXED);
    const VkTexImage *img = vk_texture_get(t);
    if (!img || !(img->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
        if (InterlockedIncrement(&n_skipped) <= 8)
            rt_log("vk: present of texture %p (format %#x) skipped: no colour image", (const void *)t, t ? t->d.format : 0);
        return;
    }
    PresentJob j = {.image = img->image, .format = img->format, .w = (int)img->width, .h = (int)img->height, .frame = frame,
                    .at = at_ns, .queued = kern_now_ns()};
    if (crop) {
        j.has_crop = 1;
        for (int i = 0; i < 4; i++) j.crop[i] = crop[i];
    }
    vk_upload_flush(0);   // an upload vk_texture_get just recorded goes before the blit
    AcquireSRWLockExclusive(&pl);
    if (!present_thread) {
        HANDLE h = CreateThread(NULL, 0, present_main, NULL, 0, NULL);
        if (!h) rt_fatal("vk: cannot start the present thread (error %lu)", GetLastError());
        CloseHandle(h);
        present_thread = 1;
    }
    while (pending) SleepConditionVariableSRW(&pcv, &pl, INFINITE, 0);   // one present in waiting at most
    job = j;
    pending = 1;
    WakeAllConditionVariable(&pcv);
    ReleaseSRWLockExclusive(&pl);
    if (rdoc_active()) { vk_present_drain(); rdoc_after_present(frame); }
}

// Pipelines (T6b) drop a released program's pipelines here.
WEAK void vk_program_release(const prog_t *p) { (void)p; }

static int vk_be_pool_init(pool_t *p) { vk_sync_start(); return vk_pool_init(p); }
static void vk_be_pool_fini(pool_t *p) { vk_sync_start(); vk_present_drain(); vk_pool_fini(p); }
static int vk_be_pool_map_virtual(pool_t *v, int n, const void *requests) {
    vk_sync_start();
    vk_present_drain();
    return vk_pool_map_virtual(v, n, requests);
}
static void vk_be_texture_register(int id, const tex_t *t, const view_t *v) { vk_sync_start(); vk_texture_register(id, t, v); }
static void vk_be_sampler_register(int id, const samp_par *s) { vk_sync_start(); vk_sampler_register(id, s); }
static void vk_be_texture_release(const tex_t *t) { vk_sync_start(); vk_present_drain(); vk_texture_release(t); }
static void vk_be_program_release(const prog_t *p) { vk_program_release(p); }
static uint64_t vk_be_submit(const uint64_t *handles, int n) {
    vk_sync_start();
    vk_present_drain();
    vk_texture_new_submit();
    return vk_sync_submit(handles, n);
}
static void vk_be_finish(void) { vk_sync_start(); vk_present_drain(); vk_sync_finish(); }
static void vk_be_read_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row,
                              uint64_t img) {
    vk_sync_start();
    vk_read_texels(t, v, r, dst, row, img);
}
static void vk_be_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row,
                               uint64_t img) {
    vk_sync_start();
    vk_present_drain();
    vk_write_texels(t, v, r, src, row, img);
}
static void vk_be_guest_written(uint64_t addr, uint64_t size) { vk_sync_start(); vk_guest_written(addr, size); }
static void vk_be_guest_read(uint64_t addr, uint64_t size) {
    if (!vk_texture_cpu_stale(addr, size)) return;
    vk_sync_start();
    vk_present_drain();
    vk_texture_write_back(addr, size);
}

const GfxBackend gfx_vulkan_backend = {
    .name = "vulkan",
    .pool_init = vk_be_pool_init,
    .pool_fini = vk_be_pool_fini,
    .pool_map_virtual = vk_be_pool_map_virtual,
    .texture_register = vk_be_texture_register,
    .sampler_register = vk_be_sampler_register,
    .texture_release = vk_be_texture_release,
    .program_release = vk_be_program_release,
    .submit = vk_be_submit,
    .fence_wait = vk_sync_fence_wait,
    .fence_done = vk_sync_fence_done,
    .present = vk_be_present,
    .finish = vk_be_finish,
    .read_texels = vk_be_read_texels,
    .write_texels = vk_be_write_texels,
    .guest_written = vk_be_guest_written,
    .guest_read = vk_be_guest_read,
};

void vk_backend_shutdown(void) {
    vk_present_drain();
    vk_sync_shutdown();
    vk_mem_shutdown();
    vk_shutdown();
}
