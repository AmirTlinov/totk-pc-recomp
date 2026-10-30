// The acceptance profile renders every game frame at 3840x2160. Diagnostics may
// opt out explicitly; those runs cannot satisfy the 4K/144 acceptance check.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "native_policy.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

extern void runtime_fail(const char *message);
extern void runtime_gpu_path_probe(const char *name);
extern void runtime_save_probe(void);
extern void runtime_swkbd_probe(void);

static int strict_mode;
static int trace_enabled;
static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static NativeFrame frames[64];

int native_policy_strict(void) { return strict_mode; }

void native_policy_init(void) {
    const char *setting = getenv("TOTK_NATIVE_4K144");
    strict_mode = !setting || strcmp(setting, "0");
    trace_enabled = getenv("TOTK_FRAME_TRACE") != NULL;
    if (!strict_mode) return;
    const struct { const char *key, *value; } settings[] = {
        {"TOTK_OUTPUT_RES", "3840x2160"}, {"TOTK_RENDER_RES", "3840x2160"},
        {"DM_PRESENT_HZ", "144"}, {"DM_PRESENT_LINK", "1"}, {"DM_PRESENT_LATENCY", "1"},
        {"TOTK_FPS60", "1"}, {"TOTK_UNCAPPED", "0"}, {"TOTK_REALTIME", "1"}, {"TOTK_CLOTH", "1"},
        {"TOTK_MFX", "0"}, {"TOTK_MFX_UPSCALE", "0"}, {"TOTK_MFX_OUTPUT", "0"},
        {"TOTK_GPU_ALLOW_SUBMIT", "1"},
        {"TOTK_GPU_NOCOMMIT", "0"}, {"TOTK_SKIP_INVISIBLE", "0"}, {"TOTK_EXP_SKIP_INVISIBLE", "0"},
        {"TOTK_SKIP_STORERS", "0"}, {"DM_FPS", "60"},
    };
    for (unsigned i = 0; i < sizeof settings / sizeof *settings; ++i)
        if (setenv(settings[i].key, settings[i].value, 1)) runtime_fail("set native render policy");
    unsetenv("TOTK_PRESENT_SKIP");
    fprintf(stderr, "[totk] native profile: scene/output 3840x2160, 144 rendered fps, no scaling or interpolation\n");
}

int native_host_check(void) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        struct utsname machine;
        uname(&machine);
        BOOL metal4 = NO;
        if (@available(macOS 26.0, *)) metal4 = device && [device supportsFamily:MTLGPUFamilyMetal4];
        BOOL native = !strcmp(machine.machine, "arm64");
        NSDictionary *report = @{@"target": @"macos-arm64", @"native_arm64": @(native),
            @"metal4": @(metal4), @"gpu": device.name ?: @"unavailable",
            @"unified_memory": @(device.hasUnifiedMemory),
            @"status": native && metal4 ? @"pass" : @"fail"};
        NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nil];
        puts([[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding].UTF8String);
        return native && metal4 ? 0 : 1;
    }
}

void native_check_output(unsigned w, unsigned h) {
    if (!strict_mode) return;
    if (w != 3840 || h != 2160) runtime_fail("native 4K output target is not 3840x2160");
}

void native_check_scene(uint64_t frame, unsigned w, unsigned h) {
    if (!strict_mode) return;
    if (w != 3840 || h != 2160) runtime_fail("native 4K scene target is not 3840x2160");
    pthread_mutex_lock(&frame_lock);
    frames[frame % 64] = (NativeFrame){.frame=frame, .scene_width=w, .scene_height=h};
    pthread_mutex_unlock(&frame_lock);
}

NativeFrame native_frame_capture(uint64_t frame, unsigned w, unsigned h, unsigned dw, unsigned dh) {
    pthread_mutex_lock(&frame_lock);
    NativeFrame result = frames[frame % 64];
    pthread_mutex_unlock(&frame_lock);
    if (result.frame != frame) result = (NativeFrame){.frame=frame};
    result.output_width=w; result.output_height=h;
    result.drawable_width=dw; result.drawable_height=dh;
    return result;
}

// Presentation callbacks may execute on different threads. Logging is opt-in,
// buffered by stdio, and uses the display timestamp rather than queue submission.
void native_presented(uint64_t ticket, NativeFrame frame, double seconds) {
    if (!trace_enabled) return;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static FILE *trace;
    static int opened;
    pthread_mutex_lock(&lock);
    if (!opened) {
        const char *path = getenv("TOTK_FRAME_TRACE");
        if (path && *path) {
            trace = fopen(path, "w");
            if (!trace) runtime_fail("cannot open frame trace");
            setvbuf(trace, NULL, _IOLBF, 4096);
            fputs("ticket,frame,present_seconds,scene_width,scene_height,output_width,output_height,drawable_width,drawable_height,strict\n", trace);
        }
        opened = 1;
    }
    if (trace) fprintf(trace, "%llu,%llu,%.9f,%u,%u,%u,%u,%u,%u,%d\n",
        (unsigned long long)ticket, (unsigned long long)frame.frame, seconds,
        frame.scene_width, frame.scene_height, frame.output_width, frame.output_height,
        frame.drawable_width, frame.drawable_height, strict_mode);
    pthread_mutex_unlock(&lock);
}

int native_run_probe(const char *name) {
    if (!strcmp(name, "--selftest") || !strcmp(name, "--host-check")) return native_host_check();
    if (!strcmp(name, "--probe-policy")) {
        extern int mfx_enabled(void), mfxup_enabled(void), mfxup_output_enabled(void);
        extern void mfx_set(int), mfxup_set(int), mfxup_output_set(int);
        setenv("TOTK_NATIVE_4K144", "1", 1);
        native_policy_init();
        if (mfx_enabled() || mfxup_enabled() || mfxup_output_enabled()) runtime_fail("native policy initial scaling");
        mfx_set(1); mfxup_set(1); mfxup_output_set(1);
        if (mfx_enabled() || mfxup_enabled() || mfxup_output_enabled()) runtime_fail("native policy runtime scaling");
        native_check_scene(7,3840,2160);
        NativeFrame f=native_frame_capture(7,3840,2160,3840,2160);
        if(f.scene_width!=3840 || native_frame_capture(8,3840,2160,3840,2160).scene_width)
            runtime_fail("frame scene identity");
        native_check_scene(71,3840,2160);
        if(native_frame_capture(7,3840,2160,3840,2160).scene_width || f.scene_width!=3840)
            runtime_fail("frame metadata lifetime");
        puts("{\"probe\":\"policy\",\"status\":\"pass\"}");
        return 0;
    }
    // A closed list prevents unknown names from selecting the donor's default probe.
    const char *gpu[] = {"midclear", "ds-clear", "copy-alias", "dispatch-chain", "storage-view",
        "texture-retire", "pool-retire", "ubo-snapshot", "linear-rt", "depth-output", "srgb-view",
        "vertex-streams", "arg-epoch", "tex-slots", "layer-merge", "cube-merge", "compressed-alias",
        "upload-batch", "ub-rename", "drv-c0", "widen", "winding", "tile", "blockrt"};
    for (unsigned i = 0; i < sizeof gpu / sizeof *gpu; ++i) {
        char mode[80];
        snprintf(mode, sizeof mode, "--probe-gpu-%s", gpu[i]);
        if (!strcmp(name, mode)) {
            if (native_host_check()) return 1;
            setenv("TOTK_PROBE_GPU", "1", 1);
            setenv("TOTK_HEADLESS", "1", 1);
            runtime_gpu_path_probe(mode + 2);
            return 0;
        }
    }
    if (!strcmp(name, "--probe-save")) { runtime_save_probe(); return 0; }
    if (!strcmp(name, "--probe-swkbd")) { runtime_swkbd_probe(); return 0; }
    return -1;
}
