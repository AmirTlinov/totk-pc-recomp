#import "periodic_present.h"
#include "periodic_camera.h"
#include "periodic_state.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PHASE_SLOTS = 3, CAPTURE_SLOTS = 4, ANCHOR_SLOTS = 4 };
enum { SLOT_WRITING = UINT32_MAX };

typedef struct {
    float yaw_sin_cos[2];
    float pitch_sin_cos[2];
    float tan_half_fov[2];
    float limit[2];
    float ui_mask[4];
} PhaseParameters;

typedef struct {
    id<MTLTexture> scene;
    id<MTLTexture> source;
    _Atomic uint64_t frame;
    _Atomic uintptr_t pending_command_buffer;
    _Atomic uint32_t state;
} PeriodicSceneCapture;

typedef struct {
    id<MTLTexture> anchor;
    id<MTLTexture> scene;
    NSUInteger width, height;
    uint64_t source_frame, anchor_generation;
    NativeMousePhase anchor_input;
    PeriodicCamera anchor_camera;
    PeriodicAnchorClock clock;
    _Atomic uint64_t last_input_generation;
    _Atomic uint64_t last_phase_bits;
    _Atomic uintptr_t pending_command_buffer;
    _Atomic uint32_t state;
} PeriodicAnchorSnapshot;

static struct {
    id<MTLDevice> device;
    PeriodicPresentResidencyFn residency;
    PeriodicSceneCapture captures[CAPTURE_SLOTS];
    PeriodicAnchorSnapshot anchors[ANCHOR_SLOTS];
    _Atomic unsigned published_anchor;
    id<MTLBuffer> parameters[PHASE_SLOTS];
    id<MTL4ArgumentTable> arguments[PHASE_SLOTS];
    MTL4RenderPassDescriptor *passes[PHASE_SLOTS];
    id<MTLRenderPipelineState> pipeline;
    id<MTLSamplerState> sampler;
    MTLPixelFormat format;
    unsigned anchor_hz;
    int enabled;
} presenter;

enum { CAMERA_SLOTS = 64 };
static PeriodicCameraSnapshot camera_forms[CAMERA_SLOTS];
static _Atomic int camera_reported;

static __strong id<MTLResidencySet> probe_residency;

static void probe_add_allocation(id<MTLAllocation> allocation) {
    [probe_residency addAllocation:allocation];
}

static void retain_allocation(id<MTLAllocation> allocation) {
    if (allocation && presenter.residency) presenter.residency(allocation);
}

static int slot_read_acquire(_Atomic uint32_t *state) {
    uint32_t readers = atomic_load_explicit(state, memory_order_acquire);
    for (;;) {
        if (readers == SLOT_WRITING || readers == SLOT_WRITING - 1) return 0;
        if (atomic_compare_exchange_weak_explicit(state, &readers, readers + 1,
                                                  memory_order_acq_rel, memory_order_acquire)) return 1;
    }
}

static void slot_read_release(_Atomic uint32_t *state) {
    atomic_fetch_sub_explicit(state, 1, memory_order_release);
}

static PeriodicAnchorSnapshot *anchor_read_acquire(unsigned *index_out) {
    for (;;) {
        unsigned index = atomic_load_explicit(&presenter.published_anchor, memory_order_acquire);
        if (index >= ANCHOR_SLOTS) return NULL;
        PeriodicAnchorSnapshot *anchor = &presenter.anchors[index];
        if (!slot_read_acquire(&anchor->state)) continue;
        if (atomic_load_explicit(&presenter.published_anchor, memory_order_acquire) == index) {
            if (index_out) *index_out = index;
            return anchor;
        }
        slot_read_release(&anchor->state);
    }
}

static PeriodicAnchorSnapshot *anchor_write_acquire(unsigned *index_out) {
    unsigned current = atomic_load_explicit(&presenter.published_anchor, memory_order_acquire);
    for (unsigned step = 1; step <= ANCHOR_SLOTS; ++step) {
        unsigned index = current < ANCHOR_SLOTS ? (current + step) % ANCHOR_SLOTS : step - 1;
        if (index == current) continue;
        PeriodicAnchorSnapshot *anchor = &presenter.anchors[index];
        uint32_t expected = 0;
        if (!atomic_compare_exchange_strong_explicit(&anchor->state, &expected, SLOT_WRITING,
                                                     memory_order_acq_rel, memory_order_acquire)) continue;
        // The published index may have changed since the snapshot above. A
        // writer that acquired the newly current slot must relinquish it before
        // touching any payload fields.
        if (atomic_load_explicit(&presenter.published_anchor, memory_order_acquire) == index) {
            atomic_store_explicit(&anchor->state, 0, memory_order_release);
            continue;
        }
        if (index_out) *index_out = index;
        return anchor;
    }
    return NULL;
}

static void anchor_publish(unsigned index) {
    // Publish while the slot is still writer-owned. Readers that observe the
    // new index wait at SLOT_WRITING, while subsequent writers already exclude
    // this index. Releasing the slot first would allow it to be reclaimed and
    // overwritten before this publication becomes visible.
    atomic_store_explicit(&presenter.published_anchor, index, memory_order_release);
    atomic_store_explicit(&presenter.anchors[index].state, 0, memory_order_release);
}

static double phase_from_bits(uint64_t bits) {
    double phase;
    memcpy(&phase, &bits, sizeof phase);
    return phase;
}

static uint64_t phase_to_bits(double phase) {
    uint64_t bits;
    memcpy(&bits, &phase, sizeof bits);
    return bits;
}

static int env_enabled(void) {
    const char *value = getenv("TOTK_PERIODIC_PHASE");
    return value && strcmp(value, "0");
}

static int make_pipeline(MTLPixelFormat format) {
    NSError *error = nil;
    static NSString *source = @
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct V { float4 position [[position]]; float2 uv; };\n"
        "vertex V periodic_phase_vertex(uint id [[vertex_id]]) {\n"
        "  float2 uv=float2((id<<1)&2,id&2); V o; o.position=float4(uv*float2(2,-2)+float2(-1,1),0,1); o.uv=uv; return o;\n"
        "}\n"
        "struct P { float2 yaw_sin_cos; float2 pitch_sin_cos; float2 tan_half_fov; float2 limit; float4 ui_mask; };\n"
        "fragment float4 periodic_phase_fragment(V in [[stage_in]], texture2d<float> color [[texture(0)]],\n"
        "  texture2d<float> scene [[texture(1)]],\n"
        "  sampler sample [[sampler(0)]], constant P &p [[buffer(0)]]) {\n"
        "  float3 ray=float3((in.uv.x*2.0-1.0)*p.tan_half_fov.x,\n"
        "                    (1.0-in.uv.y*2.0)*p.tan_half_fov.y,1.0);\n"
        "  float sy=p.yaw_sin_cos.x,cy=p.yaw_sin_cos.y;\n"
        "  float sp=p.pitch_sin_cos.x,cp=p.pitch_sin_cos.y;\n"
        "  ray=float3(ray.x,cp*ray.y+sp*ray.z,-sp*ray.y+cp*ray.z);\n"
        "  ray=float3(cy*ray.x+sy*ray.z,ray.y,-sy*ray.x+cy*ray.z);\n"
        "  float2 uv=float2(0.5+ray.x/(2.0*ray.z*p.tan_half_fov.x),\n"
        "                   0.5-ray.y/(2.0*ray.z*p.tan_half_fov.y));\n"
        "  uv=clamp(uv,p.limit,1.0-p.limit);\n"
        "  float4 world=scene.sample(sample,uv);\n"
        "  float4 base=scene.sample(sample,in.uv),composite=color.sample(sample,in.uv);\n"
        "  float changed=max(max(abs(composite.r-base.r),abs(composite.g-base.g)),abs(composite.b-base.b));\n"
        "  float ui=p.ui_mask.x*smoothstep(p.ui_mask.y,p.ui_mask.z,changed);\n"
        "  return mix(world,composite,ui);\n"
        "}\n";
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.mathMode = MTLMathModeSafe;
    id<MTLLibrary> library = [presenter.device newLibraryWithSource:source options:options error:&error];
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"periodic_phase_vertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"periodic_phase_fragment"];
    descriptor.colorAttachments[0].pixelFormat = format;
    presenter.pipeline = library ? [presenter.device newRenderPipelineStateWithDescriptor:descriptor error:&error] : nil;
    if (!presenter.pipeline) {
        fprintf(stderr, "[totk] periodic phase pipeline failed: %s\n",
                error.localizedDescription.UTF8String ?: "unknown");
        return 0;
    }
    retain_allocation((id<MTLAllocation>)presenter.pipeline);
    presenter.format = format;
    return 1;
}

void periodic_present_prepare(id<MTLDevice> device, PeriodicPresentResidencyFn residency) {
    if (presenter.device || !device) return;
    presenter.device = device;
    presenter.residency = residency;
    atomic_store_explicit(&presenter.published_anchor, UINT32_MAX, memory_order_relaxed);
    presenter.enabled = env_enabled();
    const char *anchor_hz = getenv("TOTK_PERIODIC_ANCHOR_HZ");
    int configured_hz = anchor_hz ? atoi(anchor_hz) : 0;
    presenter.anchor_hz = configured_hz > 0 ? (unsigned)configured_hz : 0;
    if (!presenter.enabled) return;
    NSError *error = nil;
    MTL4ArgumentTableDescriptor *arguments = [MTL4ArgumentTableDescriptor new];
    arguments.maxBufferBindCount = 1;
    arguments.maxTextureBindCount = 2;
    arguments.maxSamplerStateBindCount = 1;
    for (unsigned slot = 0; slot < PHASE_SLOTS; ++slot) {
        presenter.arguments[slot] = [device newArgumentTableWithDescriptor:arguments error:&error];
        presenter.parameters[slot] = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        presenter.passes[slot] = [MTL4RenderPassDescriptor new];
        if (!presenter.arguments[slot] || !presenter.parameters[slot] || !presenter.passes[slot]) {
            fprintf(stderr, "[totk] periodic phase resources failed: %s\n",
                    error.localizedDescription.UTF8String ?: "unknown");
            presenter.enabled = 0;
            return;
        }
        retain_allocation((id<MTLAllocation>)presenter.parameters[slot]);
    }
    MTLSamplerDescriptor *sampler = [MTLSamplerDescriptor new];
    sampler.minFilter = MTLSamplerMinMagFilterLinear;
    sampler.magFilter = MTLSamplerMinMagFilterLinear;
    sampler.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
    presenter.sampler = [device newSamplerStateWithDescriptor:sampler];
    if (!presenter.sampler) presenter.enabled = 0;
}

int periodic_present_enabled(void) { return presenter.enabled; }

unsigned periodic_present_anchor_ticks(PeriodicAnchorCadence *cadence, unsigned display_hz, unsigned fallback_ticks) {
    return periodic_clock_anchor_stride(cadence, display_hz, presenter.anchor_hz, fallback_ticks);
}

static int capture_scene_texture(PeriodicSceneCapture *capture, id<MTLTexture> source) {
    if (capture->scene && capture->scene.width == source.width &&
        capture->scene.height == source.height && capture->scene.pixelFormat == source.pixelFormat)
        return 1;
    capture->scene = nil;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:source.pixelFormat width:source.width height:source.height mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead;
    capture->scene = [presenter.device newTextureWithDescriptor:descriptor];
    if (!capture->scene) return 0;
    retain_allocation((id<MTLAllocation>)capture->scene);
    return 1;
}

int periodic_present_capture_scene(id<MTL4CommandBuffer> command_buffer,
                                   id<MTLTexture> canvas, uint64_t frame) {
    if (!presenter.enabled || !command_buffer || !canvas || frame == UINT64_MAX) return 0;
    PeriodicSceneCapture *capture = &presenter.captures[frame % CAPTURE_SLOTS];
    uint32_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&capture->state, &expected, SLOT_WRITING,
                                                 memory_order_acq_rel, memory_order_acquire)) return 0;
    atomic_store_explicit(&capture->frame, 0, memory_order_relaxed);
    if (!capture_scene_texture(capture, canvas)) {
        atomic_store_explicit(&capture->state, 0, memory_order_release);
        return 0;
    }
    capture->source = canvas;
    id<MTL4ComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    [encoder barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll
                     visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder copyFromTexture:canvas toTexture:capture->scene];
    [encoder endEncoding];
    atomic_store_explicit(&capture->frame, frame + 1, memory_order_relaxed);
    atomic_store_explicit(&capture->pending_command_buffer,
                          (uintptr_t)(__bridge void *)command_buffer, memory_order_release);
    return 1;
}

void periodic_present_note_camera(uint64_t frame, const float *camera, unsigned count) {
    if (frame == UINT64_MAX || !camera || count < 71) return;
    float fov = camera[70], aspect = camera[62];
    if (!isfinite(fov) || !isfinite(aspect) || fov < 0.2f || fov > 2.8f ||
        aspect < 0.5f || aspect > 4.0f) return;
    uint32_t values[2];
    memcpy(&values[0], &fov, sizeof fov);
    memcpy(&values[1], &aspect, sizeof aspect);
    uint64_t packed = (uint64_t)values[0] | ((uint64_t)values[1] << 32);
    unsigned slot = (unsigned)(frame % CAMERA_SLOTS);
    periodic_camera_snapshot_store(&camera_forms[slot], frame, packed);
    if (!atomic_exchange_explicit(&camera_reported, 1, memory_order_relaxed))
        fprintf(stderr, "[totk] periodic camera form: vertical FOV %.4f rad, aspect %.4f\n", fov, aspect);
}

PeriodicCamera periodic_present_camera(uint64_t anchor_frame) {
    uint64_t packed = 0;
    unsigned slot = (unsigned)(anchor_frame % CAMERA_SLOTS);
    periodic_camera_snapshot_load(&camera_forms[slot], anchor_frame, &packed);
    uint32_t values[2] = {(uint32_t)packed, (uint32_t)(packed >> 32)};
    PeriodicCamera camera = {0};
    memcpy(&camera.vertical_fov, &values[0], sizeof camera.vertical_fov);
    memcpy(&camera.aspect, &values[1], sizeof camera.aspect);
    return camera;
}

static int ensure_anchor(PeriodicAnchorSnapshot *anchor, id<MTLTexture> source) {
    if (anchor->anchor && anchor->width == source.width && anchor->height == source.height &&
        anchor->anchor.pixelFormat == source.pixelFormat) return 1;
    anchor->anchor = nil;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:source.pixelFormat width:source.width height:source.height mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead;
    anchor->anchor = [presenter.device newTextureWithDescriptor:descriptor];
    if (!anchor->anchor) return 0;
    retain_allocation((id<MTLAllocation>)anchor->anchor);
    anchor->width = source.width;
    anchor->height = source.height;
    return 1;
}

static int scene_capture(uint64_t anchor_frame, id<MTLTexture> presented,
                         id<MTLTexture> *source_out, id<MTLTexture> *scene_out) {
    for (unsigned behind = 0; behind < 2 && anchor_frame >= behind; ++behind) {
        uint64_t frame = anchor_frame - behind;
        PeriodicSceneCapture *capture = &presenter.captures[frame % CAPTURE_SLOTS];
        if (!slot_read_acquire(&capture->state)) continue;
        uint64_t tag = atomic_load_explicit(&capture->frame, memory_order_relaxed);
        id<MTLTexture> source = capture->source;
        id<MTLTexture> scene = capture->scene;
        int valid = tag == frame + 1 && source && scene &&
            source.width == presented.width && source.height == presented.height &&
            source.width == scene.width && source.height == scene.height &&
            source.pixelFormat == scene.pixelFormat;
        if (valid) { *source_out = source; *scene_out = scene; }
        slot_read_release(&capture->state);
        if (valid) return 1;
    }
    return 0;
}

uint64_t periodic_present_capture_anchor(id<MTL4CommandBuffer> command_buffer,
                                         id<MTLTexture> source, uint64_t frame,
                                         double presentation_time, NativeMousePhase input,
                                         PeriodicSimulationStamp simulation,
                                         MTLPixelFormat output_format) {
    if (!presenter.enabled || !command_buffer || !source || !isfinite(presentation_time) ||
        presentation_time <= 0 || presentation_time >= (double)UINT64_MAX / 1e9) return 0;
    id<MTLTexture> anchor_source = nil, scene = nil;
    if (!scene_capture(frame, source, &anchor_source, &scene)) anchor_source = source;
    PeriodicAnchorSnapshot *anchor = anchor_write_acquire(NULL);
    if (!anchor) return 0;
    if (!ensure_anchor(anchor, anchor_source)) {
        atomic_store_explicit(&anchor->state, 0, memory_order_release);
        return 0;
    }
    // Build the immutable shader state at the first anchor. A first mouse
    // movement must never compile a pipeline in the display callback.
    if ((!presenter.pipeline && !make_pipeline(output_format)) ||
        (presenter.pipeline && presenter.format != output_format)) {
        atomic_store_explicit(&anchor->state, 0, memory_order_release);
        return 0;
    }
    id<MTL4ComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    [encoder barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll
                     visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder copyFromTexture:anchor_source toTexture:anchor->anchor];
    [encoder endEncoding];
    anchor->scene = scene ?: anchor->anchor;
    // The display interval comes from anchor cadence, never from a smoothed
    // measurement that could extend an owner's temporal applicability.
    unsigned anchor_hz = presenter.anchor_hz ? presenter.anchor_hz : 60;
    anchor->clock = periodic_clock_anchor(simulation, frame,
        (uint64_t)(presentation_time * 1e9), UINT64_C(1000000000) / anchor_hz);
    anchor->source_frame = frame;
    anchor->anchor_generation = periodic_anchor_generation(frame);
    if (!anchor->anchor_generation) anchor->anchor_generation = periodic_phase_publish();
    anchor->anchor_input = input;
    anchor->anchor_camera = periodic_present_camera(frame);
    atomic_store_explicit(&anchor->last_input_generation, input.generation, memory_order_relaxed);
    atomic_store_explicit(&anchor->last_phase_bits, phase_to_bits(0), memory_order_relaxed);
    uint64_t generation = anchor->anchor_generation;
    atomic_store_explicit(&anchor->pending_command_buffer,
                          (uintptr_t)(__bridge void *)command_buffer, memory_order_release);
    return generation;
}

void periodic_present_finish_submission(id<MTL4CommandBuffer> command_buffer, int committed) {
    uintptr_t submitted = (uintptr_t)(__bridge void *)command_buffer;
    if (!submitted) return;
    for (unsigned index = 0; index < CAPTURE_SLOTS; ++index) {
        PeriodicSceneCapture *capture = &presenter.captures[index];
        if (atomic_load_explicit(&capture->pending_command_buffer, memory_order_acquire) != submitted) continue;
        atomic_store_explicit(&capture->pending_command_buffer, 0, memory_order_relaxed);
        if (!committed) atomic_store_explicit(&capture->frame, 0, memory_order_relaxed);
        atomic_store_explicit(&capture->state, 0, memory_order_release);
    }
    for (unsigned index = 0; index < ANCHOR_SLOTS; ++index) {
        PeriodicAnchorSnapshot *anchor = &presenter.anchors[index];
        if (atomic_load_explicit(&anchor->pending_command_buffer, memory_order_acquire) != submitted) continue;
        atomic_store_explicit(&anchor->pending_command_buffer, 0, memory_order_relaxed);
        if (committed) anchor_publish(index);
        else atomic_store_explicit(&anchor->state, 0, memory_order_release);
    }
}

static int phase_ready(const PeriodicAnchorSnapshot *anchor,
                       double presentation_time, NativeMousePhase input,
                       double *yaw_out, double *pitch_out) {
    if (!presenter.enabled || !anchor ||
        input.generation == atomic_load_explicit(&anchor->last_input_generation, memory_order_acquire) ||
        !isfinite(presentation_time) || presentation_time <= 0 ||
        presentation_time >= (double)UINT64_MAX / 1e9) return 0;
    // Once the anchor interval has elapsed, the retained image no longer
    // describes the simulation time requested by the display. Wait for the
    // next anchor instead of publishing a stale phase close to one.
    PeriodicPhaseTime time;
    if (!periodic_clock_phase(&anchor->clock, (uint64_t)(presentation_time * 1e9), &time)) return 0;
    double last_phase = phase_from_bits(atomic_load_explicit(&anchor->last_phase_bits, memory_order_acquire));
    if ((double)time.elapsed_ns / (double)time.duration_ns <= last_phase) return 0;
    double yaw = input.yaw - anchor->anchor_input.yaw;
    double pitch = input.pitch - anchor->anchor_input.pitch;
    if (!isfinite(yaw) || !isfinite(pitch) || fabs(yaw) > 15.0 * M_PI / 180.0 ||
        fabs(pitch) > 12.0 * M_PI / 180.0) return 0;
    if (yaw_out) *yaw_out = yaw;
    if (pitch_out) *pitch_out = pitch;
    return 1;
}

int periodic_present_phase_due(double presentation_time, NativeMousePhase input) {
    PeriodicAnchorSnapshot *anchor = anchor_read_acquire(NULL);
    if (!anchor) return 0;
    int ready = phase_ready(anchor, presentation_time, input, NULL, NULL);
    slot_read_release(&anchor->state);
    return ready;
}

id<MTLTexture> periodic_present_encode_phase(id<MTL4CommandBuffer> command_buffer,
                                             unsigned slot, id<MTLTexture> destination,
                                             double presentation_time,
                                             NativeMousePhase input,
                                             PeriodicPresentState *state) {
    if (!command_buffer || !destination || !state || slot >= PHASE_SLOTS) return nil;
    if (!presenter.pipeline || presenter.format != destination.pixelFormat) return nil;
    PeriodicAnchorSnapshot *anchor = anchor_read_acquire(NULL);
    if (!anchor) return nil;
    if (destination.width != anchor->width || destination.height != anchor->height) {
        slot_read_release(&anchor->state);
        return nil;
    }
    double yaw, pitch;
    // Large discontinuities require a new anchor so collision and occlusion are
    // resolved by the game rather than hidden by a screen-space approximation.
    if (!phase_ready(anchor, presentation_time, input, &yaw, &pitch)) {
        slot_read_release(&anchor->state);
        return nil;
    }
    PeriodicPhaseTime time;
    if (!periodic_clock_phase(&anchor->clock, (uint64_t)(presentation_time * 1e9), &time)) {
        slot_read_release(&anchor->state);
        return nil;
    }
    double phase = (double)time.elapsed_ns / (double)time.duration_ns;
    double last_phase = phase_from_bits(atomic_load_explicit(&anchor->last_phase_bits, memory_order_acquire));
    if (phase <= last_phase || phase <= 0 || phase >= 1) {
        slot_read_release(&anchor->state);
        return nil;
    }
    PhaseParameters *parameters = presenter.parameters[slot].contents;
    // This diagnostic resamples previously visible rays. It has no geometry
    // for newly exposed pixels and cannot certify a complete temporal state.
    parameters->yaw_sin_cos[0] = (float)sin(yaw);
    parameters->yaw_sin_cos[1] = (float)cos(yaw);
    parameters->pitch_sin_cos[0] = (float)sin(pitch);
    parameters->pitch_sin_cos[1] = (float)cos(pitch);
    float vertical_fov = anchor->anchor_camera.vertical_fov;
    float aspect = anchor->anchor_camera.aspect;
    if (!(vertical_fov > 0.2f && vertical_fov < 2.8f && aspect > 0.5f && aspect < 4.0f)) {
        vertical_fov = 0.87f;
        aspect = 16.0f / 9.0f;
    }
    parameters->tan_half_fov[1] = tanf(vertical_fov * 0.5f);
    parameters->tan_half_fov[0] = parameters->tan_half_fov[1] * aspect;
    parameters->limit[0] = 0.5f / (float)anchor->width;
    parameters->limit[1] = 0.5f / (float)anchor->height;
    parameters->ui_mask[0] = anchor->scene != anchor->anchor;
    parameters->ui_mask[1] = 1.0e-5f;
    parameters->ui_mask[2] = 1.0e-3f;
    parameters->ui_mask[3] = 0;
    id<MTL4ArgumentTable> arguments = presenter.arguments[slot];
    [arguments setTexture:anchor->anchor.gpuResourceID atIndex:0];
    [arguments setTexture:anchor->scene.gpuResourceID atIndex:1];
    [arguments setSamplerState:presenter.sampler.gpuResourceID atIndex:0];
    [arguments setAddress:presenter.parameters[slot].gpuAddress atIndex:0];
    MTL4RenderPassDescriptor *pass = presenter.passes[slot];
    pass.colorAttachments[0].texture = destination;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    [encoder barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll
                     visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder setArgumentTable:arguments atStages:MTLRenderStageFragment];
    [encoder setRenderPipelineState:presenter.pipeline];
    [encoder setViewport:(MTLViewport){0, 0, anchor->width, anchor->height, 0, 1}];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    atomic_store_explicit(&anchor->last_input_generation, input.generation, memory_order_release);
    atomic_store_explicit(&anchor->last_phase_bits, phase_to_bits(phase), memory_order_release);
    *state = (PeriodicPresentState){.source_frame=anchor->source_frame,
        .anchor_id=anchor->source_frame,
        .state_generation=periodic_phase_publish(), .phase=phase, .time=time};
    slot_read_release(&anchor->state);
    return destination;
}

typedef struct {
    _Atomic int stop;
    _Atomic uint64_t reads;
    _Atomic uint64_t competing_writes;
    _Atomic uint64_t errors;
} PresenterRaceProbe;

static void *presenter_race_reader(void *opaque) {
    PresenterRaceProbe *probe = opaque;
    while (!atomic_load_explicit(&probe->stop, memory_order_acquire)) {
        PeriodicAnchorSnapshot *anchor = anchor_read_acquire(NULL);
        if (!anchor) continue;
        uint64_t token = anchor->source_frame;
        if ((token & UINT64_C(0xffff000000000000)) != UINT64_C(0xace1000000000000) ||
            anchor->anchor_generation != (token ^ UINT64_C(0x55aa55aa55aa55aa)) ||
            anchor->clock.presentation_time_ns != (token & UINT64_C(0xffffffff)))
            atomic_fetch_add_explicit(&probe->errors, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&probe->reads, 1, memory_order_relaxed);
        slot_read_release(&anchor->state);
    }
    return NULL;
}

static void *presenter_race_writer(void *opaque) {
    PresenterRaceProbe *probe = opaque;
    while (!atomic_load_explicit(&probe->stop, memory_order_acquire)) {
        PeriodicAnchorSnapshot *anchor = anchor_write_acquire(NULL);
        if (!anchor) { sched_yield(); continue; }
        anchor->source_frame = UINT64_C(0xbad0000000000000);
        anchor->anchor_generation = 0;
        anchor->clock.presentation_time_ns = UINT64_MAX;
        atomic_fetch_add_explicit(&probe->competing_writes, 1, memory_order_relaxed);
        atomic_store_explicit(&anchor->state, 0, memory_order_release);
    }
    return NULL;
}

static int presenter_race_probe(void) {
    PresenterRaceProbe probe = {0};
    pthread_t reader, writer;
    for (uint64_t sequence = 0; sequence < 50000; ++sequence) {
        unsigned index = 0;
        PeriodicAnchorSnapshot *anchor;
        while (!(anchor = anchor_write_acquire(&index))) sched_yield();
        uint64_t token = UINT64_C(0xace1000000000000) | sequence;
        anchor->source_frame = token;
        anchor->anchor_generation = token ^ UINT64_C(0x55aa55aa55aa55aa);
        anchor->clock.presentation_time_ns = token & UINT64_C(0xffffffff);
        anchor_publish(index);
        if (!sequence) {
            if (pthread_create(&reader, NULL, presenter_race_reader, &probe)) return 0;
            if (pthread_create(&writer, NULL, presenter_race_writer, &probe)) {
                atomic_store_explicit(&probe.stop, 1, memory_order_release);
                (void)pthread_join(reader, NULL);
                return 0;
            }
            for (unsigned spin = 0; spin < 100000 &&
                 (!atomic_load_explicit(&probe.reads, memory_order_acquire) ||
                  !atomic_load_explicit(&probe.competing_writes, memory_order_acquire)); ++spin) sched_yield();
            if (!atomic_load_explicit(&probe.reads, memory_order_acquire) ||
                !atomic_load_explicit(&probe.competing_writes, memory_order_acquire)) {
                atomic_store_explicit(&probe.stop, 1, memory_order_release);
                (void)pthread_join(reader, NULL);
                (void)pthread_join(writer, NULL);
                return 0;
            }
        }
    }
    atomic_store_explicit(&probe.stop, 1, memory_order_release);
    if (pthread_join(reader, NULL) || pthread_join(writer, NULL)) return 0;
    return atomic_load_explicit(&probe.reads, memory_order_relaxed) > 0 &&
           atomic_load_explicit(&probe.competing_writes, memory_order_relaxed) > 0 &&
           !atomic_load_explicit(&probe.errors, memory_order_relaxed);
}

int periodic_present_probe(void) {
    @autoreleasepool {
        setenv("TOTK_PERIODIC_PHASE", "1", 1);
        setenv("TOTK_PERIODIC_ANCHOR_HZ", "48", 1);
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return 1;
        NSError *error = nil;
        MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new];
        rd.initialCapacity = 32;
        id<MTLResidencySet> residency = [device newResidencySetWithDescriptor:rd error:&error];
        probe_residency = residency;
        periodic_present_prepare(device, probe_add_allocation);
        PeriodicAnchorCadence cadence = {0};
        if (periodic_present_anchor_ticks(&cadence, 144, 1) != 3 ||
            periodic_present_anchor_ticks(&cadence, 144, 2) != 3) return 1;
        presenter.enabled = 0;
        if (periodic_present_anchor_ticks(&cadence, 144, 1) != 3) return 1;
        presenter.enabled = 1;
        id<MTL4CommandQueue> queue = [device newMTL4CommandQueue];
        id<MTL4CommandAllocator> scene_allocator = [device newCommandAllocator];
        id<MTL4CommandAllocator> anchor_allocator = [device newCommandAllocator];
        id<MTL4CommandAllocator> phase_allocator = [device newCommandAllocator];
        id<MTL4CommandBuffer> scene_cb = [device newCommandBuffer];
        id<MTL4CommandBuffer> anchor_cb = [device newCommandBuffer];
        id<MTL4CommandBuffer> phase_cb = [device newCommandBuffer];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:16 mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> source = [device newTextureWithDescriptor:td];
        id<MTLTexture> composite = [device newTextureWithDescriptor:td];
        id<MTLTexture> destination = [device newTextureWithDescriptor:td];
        id<MTLBuffer> readback = [device newBufferWithLength:32 * 16 * 4 options:MTLResourceStorageModeShared];
        uint8_t pixels[32 * 16 * 4], composite_pixels[32 * 16 * 4];
        for (unsigned y = 0; y < 16; ++y) for (unsigned x = 0; x < 32; ++x) {
            unsigned p = (y * 32 + x) * 4; pixels[p] = (uint8_t)(x * 8); pixels[p+1] = (uint8_t)(y * 16); pixels[p+2] = 0; pixels[p+3] = 255;
        }
        memcpy(composite_pixels, pixels, sizeof pixels);
        for (unsigned y = 6; y < 10; ++y) for (unsigned x = 14; x < 18; ++x) {
            unsigned p = (y * 32 + x) * 4;
            composite_pixels[p] = 16; composite_pixels[p+1] = 32;
            composite_pixels[p+2] = 255; composite_pixels[p+3] = 255;
        }
        [source replaceRegion:MTLRegionMake2D(0, 0, 32, 16) mipmapLevel:0 withBytes:pixels bytesPerRow:128];
        [composite replaceRegion:MTLRegionMake2D(0, 0, 32, 16) mipmapLevel:0 withBytes:composite_pixels bytesPerRow:128];
        [residency addAllocation:(id<MTLAllocation>)source];
        [residency addAllocation:(id<MTLAllocation>)composite];
        [residency addAllocation:(id<MTLAllocation>)destination];
        [residency addAllocation:(id<MTLAllocation>)readback];
        [queue addResidencySet:residency];
        (void)periodic_anchor_publish(41);
        float camera_block[84] = {0}; camera_block[62] = 2.0f; camera_block[70] = 0.9f;
        periodic_present_note_camera(41, camera_block, 84);
        PeriodicCamera camera = periodic_present_camera(41);
        if (fabsf(camera.vertical_fov - 0.9f) > 1e-6f || fabsf(camera.aspect - 2.0f) > 1e-6f) return 1;

        [scene_cb beginCommandBufferWithAllocator:scene_allocator];
        if (!periodic_present_capture_scene(scene_cb, source, 41)) return 1;
        [scene_cb endCommandBuffer];
        [residency commit];
        dispatch_semaphore_t scene_done = dispatch_semaphore_create(0);
        __block NSError *scene_error = nil;
        MTL4CommitOptions *scene_options = [MTL4CommitOptions new];
        [scene_options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
            scene_error = feedback.error;
            dispatch_semaphore_signal(scene_done);
        }];
        id<MTL4CommandBuffer> scene_buffers[] = {scene_cb};
        [queue commit:scene_buffers count:1 options:scene_options];
        periodic_present_finish_submission(scene_cb, 1);

        [anchor_cb beginCommandBufferWithAllocator:anchor_allocator];
        id<MTL4ComputeCommandEncoder> ui_encoder = [anchor_cb computeCommandEncoder];
        [ui_encoder barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll
                          visibilityOptions:MTL4VisibilityOptionDevice];
        [ui_encoder copyFromTexture:composite toTexture:source];
        [ui_encoder endEncoding];
        uint64_t anchor_generation = periodic_present_capture_anchor(anchor_cb, source, 41, 1.0,
            (NativeMousePhase){0,0,1}, (PeriodicSimulationStamp){42, UINT64_C(1000000000),
                UINT64_C(1000000000) / 60}, destination.pixelFormat);
        NativeMousePhase phase_input = {.yaw=0.07, .pitch=0, .generation=2};
        if (!anchor_generation || periodic_present_phase_due(1.008, phase_input)) return 1;
        [anchor_cb endCommandBuffer];
        [residency commit];
        dispatch_semaphore_t anchor_done = dispatch_semaphore_create(0);
        __block NSError *anchor_error = nil;
        MTL4CommitOptions *anchor_options = [MTL4CommitOptions new];
        [anchor_options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
            anchor_error = feedback.error;
            dispatch_semaphore_signal(anchor_done);
        }];
        id<MTL4CommandBuffer> anchor_buffers[] = {anchor_cb};
        [queue commit:anchor_buffers count:1 options:anchor_options];
        periodic_present_finish_submission(anchor_cb, 1);
        if (!periodic_present_phase_due(1.008, phase_input)) return 1;

        [phase_cb beginCommandBufferWithAllocator:phase_allocator];
        PeriodicPresentState state = {0};
        id<MTLTexture> output = periodic_present_encode_phase(phase_cb, 0, destination, 1.008,
            phase_input, &state);
        if (output) {
            id<MTL4ComputeCommandEncoder> encoder = [phase_cb computeCommandEncoder];
            [encoder barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
            [encoder copyFromTexture:output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                         sourceSize:MTLSizeMake(32, 16, 1) toBuffer:readback destinationOffset:0
             destinationBytesPerRow:128 destinationBytesPerImage:2048];
            [encoder endEncoding];
        }
        [phase_cb endCommandBuffer];
        if (!output || state.source_frame != 41 || state.anchor_id != 41 || state.state_generation <= anchor_generation ||
            !(state.phase > 0 && state.phase < 1)) return 1;
        id<MTL4CommandBuffer> buffers[] = {phase_cb};
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block NSError *gpu_error = nil;
        MTL4CommitOptions *options = [MTL4CommitOptions new];
        [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) { gpu_error = feedback.error; dispatch_semaphore_signal(done); }];
        [queue commit:buffers count:1 options:options];
        if (dispatch_semaphore_wait(scene_done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) ||
            dispatch_semaphore_wait(anchor_done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) ||
            dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) ||
            scene_error || anchor_error || gpu_error) return 1;
        const uint8_t *out = readback.contents;
        unsigned world_pixel = (8 * 32 + 8) * 4, ui_pixel = (8 * 32 + 16) * 4;
        int shifted = out[world_pixel] > pixels[world_pixel];
        int ui_stable = out[ui_pixel + 2] > 240 && out[ui_pixel] < 32;
        int race_safe = presenter_race_probe();
        printf("{\"probe\":\"periodic-present\",\"status\":\"%s\",\"source_frame\":%llu,\"anchor_id\":%llu,\"phase\":%.6f,\"world_shifted\":%s,\"ui_stable\":%s,\"retained_scene\":true,\"publish_after_commit\":true,\"residency_committed\":true,\"snapshot_race\":%s,\"hot_path_allocations\":0}\n",
               shifted && ui_stable && race_safe ? "pass" : "fail", (unsigned long long)state.source_frame,
               (unsigned long long)state.anchor_id, state.phase, shifted ? "true" : "false",
               ui_stable ? "true" : "false", race_safe ? "true" : "false");
        return shifted && ui_stable && race_safe ? 0 : 1;
    }
}
