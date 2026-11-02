#import "periodic_ui.h"
#import <Foundation/Foundation.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { UI_RECORDING, UI_REJECTED, UI_SUBMITTED, UI_READY };
@interface PeriodicUIResources : NSObject {
@public
    id<MTL4ArgumentTable> vertex[3], fragment[3];
    MTL4RenderPassDescriptor *passes[3];
}
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLResidencySet> residency;
@property(nonatomic, strong) id<MTLResidencySet> baseResidency;
@property(nonatomic, strong) NSMutableArray *objects;
@property(nonatomic, strong) NSMutableArray<id<MTLTexture>> *sourceTextures;
@property(nonatomic, strong) NSMutableArray<id<MTLTexture>> *copiedTextures;
@property(nonatomic, strong) id<MTLTexture> base;
@property(nonatomic, strong) id<MTLTexture> result;
@property(nonatomic, strong) id<MTLTexture> canvas;
@property(nonatomic, strong) id<MTLBuffer> mismatch;
@property(nonatomic, strong) id<MTLBuffer> parameters;
@property(nonatomic) NSUInteger parameterOffset;
@property(nonatomic, strong) id<MTLComputePipelineState> comparePipeline;
@property(nonatomic, strong) id<MTL4ArgumentTable> compareArguments;
@end
@implementation PeriodicUIResources
@end

struct PeriodicUIProgram {
    _Atomic uint32_t references;
    _Atomic int state;
    uint64_t frame, canvas_owner;
    NSUInteger width, height;
    MTLPixelFormat format;
    void *resources;
    PeriodicUIDraw *draws;
    size_t count, capacity, copied_bytes;
    struct UIRange { uint64_t address, size; } *inputs;
    size_t input_count, input_capacity;
    char reason[96];
    struct PeriodicUIProgram *retired_next;
};

// GPU completion can run ahead of the display callback. Keep the bounded set
// of in-flight frame owners, so a later completion does not hide an anchor's UI.
// An acquired program remains owned even after its publication slot is reused.
enum { UI_PUBLICATIONS = 4 };
static _Atomic(PeriodicUIProgram *) published[UI_PUBLICATIONS];
static _Atomic(PeriodicUIProgram *) retired;
static _Atomic unsigned readers;

static void drain_retired(void) {
    PeriodicUIProgram *p = atomic_exchange(&retired, NULL);
    if (atomic_load(&readers)) {
        // Detach before checking readers: another publisher can retire a newer
        // program while an acquisition still refers to it.
        while (p) {
            PeriodicUIProgram *next = p->retired_next;
            PeriodicUIProgram *head = atomic_load(&retired);
            do { p->retired_next = head; } while (!atomic_compare_exchange_weak(&retired, &head, p));
            p = next;
        }
        return;
    }
    while (p) {
        PeriodicUIProgram *next = p->retired_next;
        periodic_ui_release(p); p = next;
    }
}

static void retire(PeriodicUIProgram *p) {
    if (!p) return;
    PeriodicUIProgram *head = atomic_load(&retired);
    do { p->retired_next = head; } while (!atomic_compare_exchange_weak(&retired, &head, p));
    drain_retired();
}

int periodic_ui_enabled(void) {
    static int enabled;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *value = getenv("TOTK_PERIODIC_UI");
        if (!value) value = getenv("TOTK_PERIODIC_PHASE");
        enabled = value && !strcmp(value, "1");
    });
    return enabled;
}

static PeriodicUIResources *resources(PeriodicUIProgram *p) {
    return (__bridge PeriodicUIResources *)p->resources;
}
id<MTLTexture> periodic_ui_scene(const PeriodicUIProgram *p) {
    return p ? ((__bridge PeriodicUIResources *)p->resources).base : nil;
}

void periodic_ui_hold(PeriodicUIProgram *p, id object, int resident) {
    if (!p || !object) return;
    PeriodicUIResources *r = resources(p);
    [r.objects addObject:object];
    if (resident) [r.residency addAllocation:(id<MTLAllocation>)object];
}

void periodic_ui_reject(PeriodicUIProgram *p, const char *reason) {
    if (!p || atomic_load(&p->state) != UI_RECORDING) return;
    strlcpy(p->reason, reason ? reason : "unsupported dependency", sizeof p->reason);
    atomic_store(&p->state, UI_REJECTED);
}

int periodic_ui_recording(const PeriodicUIProgram *p) {
    return p && atomic_load(&p->state) == UI_RECORDING;
}

void periodic_ui_read_range(PeriodicUIProgram *p, uint64_t address, uint64_t size) {
    if (!periodic_ui_recording(p)) return;
    if (!address || !size || address > UINT64_MAX-size) {
        periodic_ui_reject(p,"unbounded texture storage"); return;
    }
    for (size_t i=0;i<p->input_count;++i)
        if (p->inputs[i].address==address && p->inputs[i].size==size) return;
    if (p->input_count==p->input_capacity) {
        size_t capacity=p->input_capacity ? p->input_capacity*2 : 16;
        if (capacity<p->input_capacity || capacity>SIZE_MAX/sizeof *p->inputs) {
            periodic_ui_reject(p,"texture dependency storage overflow"); return;
        }
        struct UIRange *next=realloc(p->inputs,capacity*sizeof *next);
        if (!next) { periodic_ui_reject(p,"texture dependency allocation failed"); return; }
        p->inputs=next; p->input_capacity=capacity;
    }
    p->inputs[p->input_count++]=(struct UIRange){address,size};
}
void periodic_ui_write_range(PeriodicUIProgram *p, uint64_t address, uint64_t size) {
    if (!periodic_ui_recording(p) || !size) return;
    if (!address || address>UINT64_MAX-size) { periodic_ui_reject(p,"unbounded texture write"); return; }
    for (size_t i=0;i<p->input_count;++i)
        if (address<p->inputs[i].address+p->inputs[i].size && p->inputs[i].address<address+size) {
            periodic_ui_reject(p,"sampled UI texture changed during its action"); return;
        }
}

static id<MTLTexture> image(PeriodicUIProgram *p, MTLTextureUsage usage) {
    PeriodicUIResources *r = resources(p);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:p->format
        width:p->width height:p->height mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = usage;
    id<MTLTexture> t = [r.device newTextureWithDescriptor:d];
    periodic_ui_hold(p, t, 1);
    return t;
}

PeriodicUIProgram *periodic_ui_begin(id<MTL4CommandBuffer> cb, uint64_t frame,
    uint64_t canvas_owner, id<MTLTexture> canvas) {
    if (!periodic_ui_enabled() || !cb || !canvas || !canvas_owner ||
        canvas.textureType != MTLTextureType2D || canvas.sampleCount != 1) return NULL;
    drain_retired();
    PeriodicUIProgram *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    atomic_init(&p->references, 1);
    p->frame = frame; p->canvas_owner = canvas_owner;
    p->width = canvas.width; p->height = canvas.height; p->format = canvas.pixelFormat;
    PeriodicUIResources *r = [PeriodicUIResources new];
    p->resources = (void *)CFBridgingRetain(r);
    r.device = canvas.device;
    r.canvas = canvas;
    r.objects = [NSMutableArray array];
    r.sourceTextures = [NSMutableArray array];
    r.copiedTextures = [NSMutableArray array];
    NSError *error = nil;
    r.residency = [r.device newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];
    r.baseResidency = [r.device newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];
    MTL4ArgumentTableDescriptor *arguments = [MTL4ArgumentTableDescriptor new];
    arguments.maxBufferBindCount = 31;
    for (unsigned i = 0; i < 3; ++i) {
        r->vertex[i] = [r.device newArgumentTableWithDescriptor:arguments error:&error];
        r->fragment[i] = [r.device newArgumentTableWithDescriptor:arguments error:&error];
        r->passes[i] = [MTL4RenderPassDescriptor new];
        if (!r->vertex[i] || !r->fragment[i]) periodic_ui_reject(p, "argument table allocation failed");
    }
    r.base = image(p, MTLTextureUsageShaderRead);
    r.result = image(p, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
    if (!r.residency || !r.baseResidency || !r.base || !r.result) {
        periodic_ui_reject(p, "scene allocation failed");
        return p;
    }
    // This copy is on the same GPU timeline as the original scene and UI.
    [r.baseResidency addAllocation:r.base]; [r.baseResidency commit];
    [cb useResidencySet:r.baseResidency];
    id<MTL4ComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll
        visibilityOptions:MTL4VisibilityOptionDevice];
    [e copyFromTexture:canvas toTexture:r.base];
    [e endEncoding];
    return p;
}

uint64_t periodic_ui_bytes(PeriodicUIProgram *p, const void *bytes, size_t size) {
    if (!periodic_ui_recording(p) || !bytes || !size || size > (64u << 20)) {
        periodic_ui_reject(p, "unbounded buffer dependency"); return 0;
    }
    PeriodicUIResources *r = resources(p);
    NSUInteger offset = (r.parameterOffset + 15) & ~(NSUInteger)15;
    if (!r.parameters || offset > r.parameters.length || size > r.parameters.length - offset) {
        // All draw inputs share an append-only arena. Addresses already handed
        // to a draw remain immutable when a larger input needs another segment.
        NSUInteger length = (size + 4095) & ~(NSUInteger)4095;
        if (length < (128u << 10)) length = 128u << 10;
        r.parameters = [r.device newBufferWithLength:length options:MTLResourceStorageModeShared];
        if (!r.parameters) { periodic_ui_reject(p, "parameter arena allocation failed"); return 0; }
        periodic_ui_hold(p, r.parameters, 1);
        offset = 0;
    }
    memcpy((uint8_t *)r.parameters.contents + offset, bytes, size);
    r.parameterOffset = offset + size;
    p->copied_bytes += size;
    return r.parameters.gpuAddress + offset;
}

uint64_t periodic_ui_texture(PeriodicUIProgram *p, id<MTLTexture> texture) {
    if (!periodic_ui_recording(p) || !texture) return 0;
    PeriodicUIResources *r = resources(p);
    NSUInteger index = [r.sourceTextures indexOfObjectIdenticalTo:texture];
    if (index != NSNotFound) return r.copiedTextures[index].gpuResourceID._impl;
    // Handle simple sampled images first. Buffer-backed, sparse and multisample
    // dependencies require an explicit snapshot law before they can participate.
    if (texture.sampleCount != 1 || (texture.textureType != MTLTextureType2D && texture.textureType != MTLTextureType2DArray) ||
        texture.buffer || texture.framebufferOnly) {
        periodic_ui_reject(p, "unsupported texture dependency"); return 0;
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = texture.textureType; d.pixelFormat = texture.pixelFormat;
    d.width = texture.width; d.height = texture.height; d.depth = texture.depth;
    d.mipmapLevelCount = texture.mipmapLevelCount; d.arrayLength = texture.arrayLength;
    d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead;
    d.swizzle = texture.swizzle;
    id<MTLTexture> copy = [r.device newTextureWithDescriptor:d];
    if (!copy) { periodic_ui_reject(p, "texture allocation failed"); return 0; }
    [r.sourceTextures addObject:texture]; [r.copiedTextures addObject:copy];
    periodic_ui_hold(p, texture, 1); periodic_ui_hold(p, copy, 1);
    return copy.gpuResourceID._impl;
}

int periodic_ui_append(PeriodicUIProgram *p, const PeriodicUIDraw *draw) {
    if (!periodic_ui_recording(p) || !draw || !draw->pipeline || !draw->count || !draw->instances) return 0;
    if (draw->cull > MTLCullModeBack || draw->winding > MTLWindingCounterClockwise ||
        draw->fill > MTLTriangleFillModeLines || draw->clip > MTLDepthClipModeClamp) {
        periodic_ui_reject(p, "unresolved UI raster state"); return 0;
    }
    if (p->count == p->capacity) {
        size_t capacity = p->capacity ? p->capacity * 2 : 128;
        if (capacity < p->capacity || capacity > SIZE_MAX / sizeof *p->draws) {
            periodic_ui_reject(p, "draw storage overflow"); return 0;
        }
        PeriodicUIDraw *next = realloc(p->draws, capacity * sizeof *next);
        if (!next) { periodic_ui_reject(p, "draw storage allocation failed"); return 0; }
        p->draws = next; p->capacity = capacity;
    }
    p->draws[p->count++] = *draw;
    periodic_ui_hold(p, draw->pipeline, 1); periodic_ui_hold(p, draw->depth, 0);
    return 1;
}

static int encode_draws(PeriodicUIProgram *p, id<MTL4CommandBuffer> cb, id<MTLTexture> target, unsigned slot) {
    if (slot >= 3) return 0;
    PeriodicUIResources *r = resources(p);
    id<MTL4ArgumentTable> vertex = r->vertex[slot], fragment = r->fragment[slot];
    if (!vertex || !fragment) return 0;
    MTL4RenderPassDescriptor *pass = r->passes[slot];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [e setArgumentTable:vertex atStages:MTLRenderStageVertex];
    [e setArgumentTable:fragment atStages:MTLRenderStageFragment];
    for (size_t i = 0; i < p->count; ++i) {
        const PeriodicUIDraw *d = &p->draws[i];
        [e setRenderPipelineState:d->pipeline]; [e setDepthStencilState:d->depth];
        [e setViewport:d->viewport]; [e setScissorRect:d->scissor];
        [e setCullMode:d->cull]; [e setFrontFacingWinding:d->winding];
        [e setTriangleFillMode:d->fill]; [e setDepthClipMode:d->clip];
        [e setDepthBias:d->bias[0] slopeScale:d->bias[1] clamp:d->bias[2]];
        [e setBlendColorRed:d->blend[0] green:d->blend[1] blue:d->blend[2] alpha:d->blend[3]];
        for (unsigned b = 0; b < 31; ++b) {
            if (d->vertex_mask & (1u << b)) [vertex setAddress:d->vertex[b] atIndex:b];
            if (d->fragment_mask & (1u << b)) [fragment setAddress:d->fragment[b] atIndex:b];
        }
        if (d->indexed) [e drawIndexedPrimitives:d->primitive indexCount:d->count indexType:d->index_type
            indexBuffer:d->index_address indexBufferLength:d->index_length instanceCount:d->instances
            baseVertex:d->base_vertex baseInstance:d->base_instance];
        else [e drawPrimitives:d->primitive vertexStart:d->first vertexCount:d->count
            instanceCount:d->instances baseInstance:d->base_instance];
    }
    [e endEncoding];
    return 1;
}

int periodic_ui_compare(PeriodicUIProgram *p, id<MTL4CommandBuffer> cb, id<MTLTexture> reference) {
    if (!periodic_ui_recording(p) || !p->count || reference.width != p->width ||
        reference.height != p->height || reference.pixelFormat != p->format) return 0;
    PeriodicUIResources *r = resources(p);
    static NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void compare_ui(texture2d<float,access::read> a [[texture(0)]],"
        "texture2d<float,access::read> b [[texture(1)]], device atomic_uint *n [[buffer(0)]],"
        "uint2 q [[thread_position_in_grid]]) { if(q.x>=a.get_width()||q.y>=a.get_height())return;"
        "float4 x=a.read(q),y=b.read(q); if(any(as_type<uint4>(x)!=as_type<uint4>(y)))"
        "atomic_fetch_add_explicit(n,1u,memory_order_relaxed); }";
    // Compile once per device on first use, before there are phase submissions.
    static id<MTLComputePipelineState> compare;
    if (!compare) {
        MTLCompileOptions *options = [MTLCompileOptions new]; options.mathMode = MTLMathModeSafe;
        id<MTLLibrary> library = [r.device newLibraryWithSource:source options:options error:nil];
        compare = [r.device newComputePipelineStateWithFunction:[library newFunctionWithName:@"compare_ui"] error:nil];
    }
    r.comparePipeline = compare;
    r.mismatch = [r.device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    if (r.mismatch) memset(r.mismatch.contents, 0, r.mismatch.length);
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new];
    ad.maxBufferBindCount = 1; ad.maxTextureBindCount = 2;
    r.compareArguments = [r.device newArgumentTableWithDescriptor:ad error:nil];
    if (!compare || !r.mismatch || !r.compareArguments) {
        periodic_ui_reject(p, "reference comparison allocation failed"); return 0;
    }
    periodic_ui_hold(p, compare, 1); periodic_ui_hold(p, r.mismatch, 1);
    periodic_ui_hold(p, reference, 1);
    [r.residency commit]; [cb useResidencySet:r.residency];
    id<MTL4ComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    for (NSUInteger i = 0; i < r.sourceTextures.count; ++i)
        [e copyFromTexture:r.sourceTextures[i] toTexture:r.copiedTextures[i]];
    [e copyFromTexture:r.base toTexture:r.result]; [e endEncoding];
    if (!encode_draws(p, cb, r.result, 0)) { periodic_ui_reject(p, "UI encoder allocation failed"); return 0; }
    e = [cb computeCommandEncoder];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [r.compareArguments setAddress:r.mismatch.gpuAddress atIndex:0];
    [r.compareArguments setTexture:r.result.gpuResourceID atIndex:0];
    [r.compareArguments setTexture:reference.gpuResourceID atIndex:1];
    [e setArgumentTable:r.compareArguments]; [e setComputePipelineState:compare];
    [e dispatchThreads:MTLSizeMake(p->width,p->height,1) threadsPerThreadgroup:MTLSizeMake(16,16,1)];
    [e endEncoding]; atomic_store(&p->state, UI_SUBMITTED);
    return 1;
}

void periodic_ui_completed(PeriodicUIProgram *p, int gpu_error) {
    if (!p) return;
    PeriodicUIResources *r = resources(p);
    uint32_t mismatch = r.mismatch ? *(const uint32_t *)r.mismatch.contents : UINT32_MAX;
    int valid = atomic_load(&p->state) == UI_SUBMITTED && !gpu_error && !mismatch;
    fprintf(stderr,"[periodic-ui] frame=%llu owner=%llu draws=%zu bytes=%zu mismatch=%u valid=%d reason=%s\n",
        (unsigned long long)p->frame,(unsigned long long)p->canvas_owner,p->count,p->copied_bytes,
        mismatch,valid,p->reason);
    if (valid) {
        atomic_store(&p->state, UI_READY);
        atomic_fetch_add(&p->references, 1);
        // GPU feedback may arrive on different threads. Older completed work
        // must not replace the published successor. The reader guard also
        // protects this comparison before either callback holds a reference.
        atomic_fetch_add(&readers, 1);
        _Atomic(PeriodicUIProgram *) *slot = &published[p->frame % UI_PUBLICATIONS];
        PeriodicUIProgram *old = atomic_load(slot);
        int installed = 0;
        while (!old || old->frame < p->frame) {
            if (atomic_compare_exchange_weak(slot, &old, p)) { installed = 1; break; }
        }
        if (installed) retire(old);
        else periodic_ui_release(p);
        atomic_fetch_sub(&readers, 1);
        drain_retired();
    }
    periodic_ui_release(p);
}

PeriodicUIProgram *periodic_ui_acquire(uint64_t frame, id<MTLTexture> canvas) {
    atomic_fetch_add(&readers, 1);
    PeriodicUIProgram *p = atomic_load(&published[frame % UI_PUBLICATIONS]);
    if (p && p->frame == frame && resources(p).canvas == canvas && atomic_load(&p->state) == UI_READY)
        atomic_fetch_add(&p->references, 1);
    else p = NULL;
    atomic_fetch_sub(&readers, 1);
    return p;
}

void periodic_ui_release(PeriodicUIProgram *p) {
    if (!p || atomic_fetch_sub(&p->references, 1) != 1) return;
    CFRelease(p->resources); free(p->draws); free(p->inputs); free(p);
}

int periodic_ui_encode(PeriodicUIProgram *p, id<MTL4CommandBuffer> cb,
    id<MTLTexture> scene, id<MTLTexture> destination, unsigned presentation_slot) {
    if (!p || !cb || presentation_slot >= 3 || atomic_load(&p->state) != UI_READY || scene.width != p->width ||
        scene.height != p->height || scene.pixelFormat != p->format ||
        destination.width != p->width || destination.height != p->height ||
        destination.pixelFormat != p->format) return 0;
    [cb useResidencySet:resources(p).residency];
    id<MTL4ComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    if (scene != destination) [e copyFromTexture:scene toTexture:destination];
    [e endEncoding];
    return encode_draws(p, cb, destination, presentation_slot);
}
