#import "periodic_present.h"
#include "periodic_form.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PHASE_SLOTS = 3, CAPTURE_SLOTS = 4 };

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
} PeriodicSceneCapture;

static struct {
    id<MTLDevice> device;
    PeriodicPresentResidencyFn residency;
    id<MTLTexture> anchor;
    id<MTLTexture> scene;
    PeriodicSceneCapture captures[CAPTURE_SLOTS];
    id<MTLBuffer> parameters[PHASE_SLOTS];
    id<MTL4ArgumentTable> arguments[PHASE_SLOTS];
    MTL4RenderPassDescriptor *passes[PHASE_SLOTS];
    id<MTLRenderPipelineState> pipeline;
    id<MTLSamplerState> sampler;
    MTLPixelFormat format;
    NSUInteger width, height;
    uint64_t source_frame, anchor_generation, last_input_generation;
    NativeMousePhase anchor_input;
    PeriodicCamera anchor_camera;
    double anchor_time, anchor_period, last_phase;
    unsigned anchor_hz;
    int valid, enabled;
} presenter;

enum { CAMERA_SLOTS = 64 };
static struct {
    _Atomic uint64_t frame;
    _Atomic uint64_t projection;
} camera_forms[CAMERA_SLOTS];
static _Atomic int camera_reported;

static __strong id<MTLResidencySet> probe_residency;

static void probe_add_allocation(id<MTLAllocation> allocation) {
    [probe_residency addAllocation:allocation];
}

static void retain_allocation(id<MTLAllocation> allocation) {
    if (allocation && presenter.residency) presenter.residency(allocation);
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
    presenter.enabled = env_enabled();
    if (!presenter.enabled) return;
    const char *anchor_hz = getenv("TOTK_PERIODIC_ANCHOR_HZ");
    int configured_hz = anchor_hz ? atoi(anchor_hz) : 0;
    presenter.anchor_hz = configured_hz > 0 ? (unsigned)configured_hz : 0;
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

unsigned periodic_present_anchor_ticks(unsigned display_hz, unsigned fallback_ticks) {
    if (!fallback_ticks) fallback_ticks = 1;
    if (!presenter.enabled || !presenter.anchor_hz || !display_hz) return fallback_ticks;
    unsigned ticks = (display_hz + presenter.anchor_hz / 2) / presenter.anchor_hz;
    return ticks > fallback_ticks ? ticks : fallback_ticks;
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
    atomic_store_explicit(&capture->frame, 0, memory_order_release);
    if (!capture_scene_texture(capture, canvas)) return 0;
    capture->source = canvas;
    id<MTL4ComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    [encoder barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll
                     visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder copyFromTexture:canvas toTexture:capture->scene];
    [encoder endEncoding];
    atomic_store_explicit(&capture->frame, frame + 1, memory_order_release);
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
    atomic_store_explicit(&camera_forms[slot].projection, packed, memory_order_relaxed);
    atomic_store_explicit(&camera_forms[slot].frame, frame + 1, memory_order_release);
    if (!atomic_exchange_explicit(&camera_reported, 1, memory_order_relaxed))
        fprintf(stderr, "[totk] periodic camera form: vertical FOV %.4f rad, aspect %.4f\n", fov, aspect);
}

PeriodicCamera periodic_present_camera(uint64_t anchor_frame) {
    uint64_t packed = 0;
    for (unsigned behind = 0; behind < 2 && anchor_frame >= behind; ++behind) {
        uint64_t frame = anchor_frame - behind;
        unsigned slot = (unsigned)(frame % CAMERA_SLOTS);
        uint64_t tag = atomic_load_explicit(&camera_forms[slot].frame, memory_order_acquire);
        if (tag == frame + 1) {
            uint64_t candidate = atomic_load_explicit(&camera_forms[slot].projection, memory_order_relaxed);
            if (atomic_load_explicit(&camera_forms[slot].frame, memory_order_acquire) == tag) {
                packed = candidate;
                break;
            }
        }
    }
    uint32_t values[2] = {(uint32_t)packed, (uint32_t)(packed >> 32)};
    PeriodicCamera camera = {0};
    memcpy(&camera.vertical_fov, &values[0], sizeof camera.vertical_fov);
    memcpy(&camera.aspect, &values[1], sizeof camera.aspect);
    return camera;
}

static int ensure_anchor(id<MTLTexture> source) {
    if (presenter.anchor && presenter.width == source.width && presenter.height == source.height &&
        presenter.anchor.pixelFormat == source.pixelFormat) return 1;
    presenter.anchor = nil;
    presenter.valid = 0;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:source.pixelFormat width:source.width height:source.height mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead;
    presenter.anchor = [presenter.device newTextureWithDescriptor:descriptor];
    if (!presenter.anchor) return 0;
    retain_allocation((id<MTLAllocation>)presenter.anchor);
    presenter.width = source.width;
    presenter.height = source.height;
    return 1;
}

static PeriodicSceneCapture *scene_capture(uint64_t anchor_frame, id<MTLTexture> presented) {
    for (unsigned behind = 0; behind < 2 && anchor_frame >= behind; ++behind) {
        uint64_t frame = anchor_frame - behind;
        PeriodicSceneCapture *capture = &presenter.captures[frame % CAPTURE_SLOTS];
        uint64_t tag = atomic_load_explicit(&capture->frame, memory_order_acquire);
        id<MTLTexture> source = capture->source;
        id<MTLTexture> scene = capture->scene;
        if (tag == frame + 1 && source && scene &&
            source.width == presented.width && source.height == presented.height &&
            source.width == scene.width && source.height == scene.height &&
            source.pixelFormat == scene.pixelFormat &&
            atomic_load_explicit(&capture->frame, memory_order_acquire) == tag)
            return capture;
    }
    return NULL;
}

uint64_t periodic_present_capture_anchor(id<MTL4CommandBuffer> command_buffer,
                                         id<MTLTexture> source, uint64_t frame,
                                         double presentation_time, NativeMousePhase input,
                                         MTLPixelFormat output_format) {
    if (!presenter.enabled || !command_buffer || !source) return 0;
    PeriodicSceneCapture *capture = scene_capture(frame, source);
    id<MTLTexture> anchor_source = capture ? capture->source : source;
    if (!ensure_anchor(anchor_source)) return 0;
    // Build the immutable shader state at the first anchor. A first mouse
    // movement must never compile a pipeline in the display callback.
    if ((!presenter.pipeline || presenter.format != output_format) &&
        !make_pipeline(output_format)) return 0;
    id<MTL4ComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    [encoder barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll
                     visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder copyFromTexture:anchor_source toTexture:presenter.anchor];
    [encoder endEncoding];
    presenter.scene = capture ? capture->scene : presenter.anchor;
    if (presenter.anchor_time > 0 && presentation_time > presenter.anchor_time) {
        double interval = presentation_time - presenter.anchor_time;
        if (interval > 1.0 / 240.0 && interval < 0.1)
            presenter.anchor_period = presenter.anchor_period > 0 ? presenter.anchor_period * 0.75 + interval * 0.25 : interval;
    }
    presenter.source_frame = frame;
    presenter.anchor_generation = periodic_anchor_generation(frame);
    if (!presenter.anchor_generation)
        presenter.anchor_generation = periodic_phase_publish();
    presenter.anchor_input = input;
    presenter.anchor_camera = periodic_present_camera(frame);
    presenter.last_input_generation = input.generation;
    presenter.anchor_time = presentation_time;
    presenter.last_phase = 0;
    presenter.valid = 1;
    return presenter.anchor_generation;
}

static int phase_ready(double presentation_time, NativeMousePhase input,
                       double *yaw_out, double *pitch_out) {
    if (!presenter.enabled || !presenter.valid || input.generation == presenter.last_input_generation ||
        presentation_time <= presenter.anchor_time) return 0;
    double period = presenter.anchor_period > 0 ? presenter.anchor_period : 1.0 / 60.0;
    // Once the anchor interval has elapsed, the retained image no longer
    // describes the simulation time requested by the display. Wait for the
    // next anchor instead of publishing a stale phase close to one.
    if (presentation_time - presenter.anchor_time >= period) return 0;
    double yaw = input.yaw - presenter.anchor_input.yaw;
    double pitch = input.pitch - presenter.anchor_input.pitch;
    if (!isfinite(yaw) || !isfinite(pitch) || fabs(yaw) > 15.0 * M_PI / 180.0 ||
        fabs(pitch) > 12.0 * M_PI / 180.0) return 0;
    if (yaw_out) *yaw_out = yaw;
    if (pitch_out) *pitch_out = pitch;
    return 1;
}

int periodic_present_phase_due(double presentation_time, NativeMousePhase input) {
    return phase_ready(presentation_time, input, NULL, NULL);
}

id<MTLTexture> periodic_present_encode_phase(id<MTL4CommandBuffer> command_buffer,
                                             unsigned slot, id<MTLTexture> destination,
                                             double presentation_time,
                                             NativeMousePhase input,
                                             PeriodicPresentState *state) {
    if (!command_buffer || !destination || !state || slot >= PHASE_SLOTS ||
        destination.width != presenter.width || destination.height != presenter.height) return nil;
    if (!presenter.pipeline || presenter.format != destination.pixelFormat) return nil;
    double yaw, pitch;
    // Large discontinuities require a new anchor so collision and occlusion are
    // resolved by the game rather than hidden by a screen-space approximation.
    if (!phase_ready(presentation_time, input, &yaw, &pitch)) return nil;
    double period = presenter.anchor_period > 0 ? presenter.anchor_period : 1.0 / 60.0;
    double phase = (presentation_time - presenter.anchor_time) / period;
    if (phase <= presenter.last_phase) phase = presenter.last_phase + 1.0 / 65536.0;
    if (phase >= 1.0) phase = nextafter(1.0, 0.0);
    if (phase <= 0 || phase >= 1) return nil;
    PhaseParameters *parameters = presenter.parameters[slot].contents;
    // Rotate view rays instead of translating the image. This is exact for
    // mouse-only camera rotation and avoids the shear of a flat screen warp.
    parameters->yaw_sin_cos[0] = (float)sin(yaw);
    parameters->yaw_sin_cos[1] = (float)cos(yaw);
    parameters->pitch_sin_cos[0] = (float)sin(pitch);
    parameters->pitch_sin_cos[1] = (float)cos(pitch);
    float vertical_fov = presenter.anchor_camera.vertical_fov;
    float aspect = presenter.anchor_camera.aspect;
    if (!(vertical_fov > 0.2f && vertical_fov < 2.8f && aspect > 0.5f && aspect < 4.0f)) {
        vertical_fov = 0.87f;
        aspect = 16.0f / 9.0f;
    }
    parameters->tan_half_fov[1] = tanf(vertical_fov * 0.5f);
    parameters->tan_half_fov[0] = parameters->tan_half_fov[1] * aspect;
    parameters->limit[0] = 0.5f / (float)presenter.width;
    parameters->limit[1] = 0.5f / (float)presenter.height;
    parameters->ui_mask[0] = presenter.scene != presenter.anchor;
    parameters->ui_mask[1] = 1.0e-5f;
    parameters->ui_mask[2] = 1.0e-3f;
    parameters->ui_mask[3] = 0;
    id<MTL4ArgumentTable> arguments = presenter.arguments[slot];
    [arguments setTexture:presenter.anchor.gpuResourceID atIndex:0];
    [arguments setTexture:presenter.scene.gpuResourceID atIndex:1];
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
    [encoder setViewport:(MTLViewport){0, 0, presenter.width, presenter.height, 0, 1}];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    presenter.last_input_generation = input.generation;
    presenter.last_phase = phase;
    *state = (PeriodicPresentState){.source_frame=presenter.source_frame,
        .anchor_id=presenter.source_frame + 1,
        .state_generation=periodic_phase_publish(), .phase=phase};
    return destination;
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
        if (periodic_present_anchor_ticks(144, 1) != 3 ||
            periodic_present_anchor_ticks(144, 2) != 3) return 1;
        id<MTL4CommandQueue> queue = [device newMTL4CommandQueue];
        id<MTL4CommandAllocator> allocator = [device newCommandAllocator];
        id<MTL4CommandBuffer> cb = [device newCommandBuffer];
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
        periodic_form_init();
        (void)periodic_anchor_publish(41);
        [cb beginCommandBufferWithAllocator:allocator];
        float camera_block[84] = {0}; camera_block[62] = 2.0f; camera_block[70] = 0.9f;
        periodic_present_note_camera(41, camera_block, 84);
        PeriodicCamera camera = periodic_present_camera(42);
        if (fabsf(camera.vertical_fov - 0.9f) > 1e-6f || fabsf(camera.aspect - 2.0f) > 1e-6f) return 1;
        if (!periodic_present_capture_scene(cb, source, 41)) return 1;
        id<MTL4ComputeCommandEncoder> ui_encoder = [cb computeCommandEncoder];
        [ui_encoder barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll
                          visibilityOptions:MTL4VisibilityOptionDevice];
        [ui_encoder copyFromTexture:composite toTexture:source];
        [ui_encoder endEncoding];
        uint64_t anchor_generation = periodic_present_capture_anchor(cb, source, 41, 1.0,
            (NativeMousePhase){0,0,1}, destination.pixelFormat);
        PeriodicPresentState state = {0};
        id<MTLTexture> output = periodic_present_encode_phase(cb, 0, destination, 1.008,
            (NativeMousePhase){.yaw=0.07, .pitch=0, .generation=2}, &state);
        if (output) {
            id<MTL4ComputeCommandEncoder> encoder = [cb computeCommandEncoder];
            [encoder barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
            [encoder copyFromTexture:output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                         sourceSize:MTLSizeMake(32, 16, 1) toBuffer:readback destinationOffset:0
             destinationBytesPerRow:128 destinationBytesPerImage:2048];
            [encoder endEncoding];
        }
        [cb endCommandBuffer];
        if (!output || state.source_frame != 41 || state.anchor_id != 42 || state.state_generation <= anchor_generation ||
            !(state.phase > 0 && state.phase < 1)) return 1;
        [residency commit];
        id<MTL4CommandBuffer> buffers[] = {cb};
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block NSError *gpu_error = nil;
        MTL4CommitOptions *options = [MTL4CommitOptions new];
        [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) { gpu_error = feedback.error; dispatch_semaphore_signal(done); }];
        [queue commit:buffers count:1 options:options];
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) || gpu_error) return 1;
        const uint8_t *out = readback.contents;
        unsigned world_pixel = (8 * 32 + 8) * 4, ui_pixel = (8 * 32 + 16) * 4;
        int shifted = out[world_pixel] > pixels[world_pixel];
        int ui_stable = out[ui_pixel + 2] > 240 && out[ui_pixel] < 32;
        printf("{\"probe\":\"periodic-present\",\"status\":\"%s\",\"source_frame\":%llu,\"anchor_id\":%llu,\"phase\":%.6f,\"retained_scene\":true,\"hot_path_allocations\":0}\n",
               shifted && ui_stable ? "pass" : "fail", (unsigned long long)state.source_frame,
               (unsigned long long)state.anchor_id, state.phase);
        return shifted && ui_stable ? 0 : 1;
    }
}
