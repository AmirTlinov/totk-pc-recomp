#include "periodic_executor.h"
#include "periodic_compiler.h"
#include "periodic_form.h"

#import <Metal/Metal.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { EXECUTOR_CACHE_SLOTS = 1024, EXECUTOR_CACHE_WAYS = 4 };
enum { CACHE_EMPTY, CACHE_COMPILING, CACHE_READY, CACHE_FAILED, CACHE_WRITING };

typedef struct {
    uint32_t op;
    uint32_t reserved;
    uint64_t fields[8];
} ExecutorPacket;

typedef struct {
    _Atomic uint32_t state;
    uint64_t key;
    uint64_t form_id;
    uint64_t buffer_generation;
    uint32_t cluster_index;
    uint32_t packet_count;
    uint32_t draw_count;
    uint32_t uses_index_buffer;
    ExecutorPacket packets[PERIODIC_ICB_PACKET_LIMIT];
    __strong id<MTLIndirectCommandBuffer> commands;
} ExecutorCache;

static struct {
    __strong id<MTLDevice> device;
    PeriodicBufferGenerationFn buffer_generation;
    PeriodicResolveBufferFn resolve_buffer;
    PeriodicMakeResidentFn make_resident;
    ExecutorCache *cache;
    _Atomic uint64_t tracked_passes, rejected_passes, queued_clusters, compiled_clusters;
    _Atomic uint64_t executed_clusters, fallback_clusters, executed_draws;
    _Atomic int active, stopping;
    pthread_t compiler;
    struct {
        __unsafe_unretained id<MTL4RenderCommandEncoder> encoder;
        PeriodicFormSnapshot form;
        const PeriodicCompiledForm *program;
        uint64_t form_id;
        uint32_t command_index, cluster_index;
        uint32_t packet_count;
        ExecutorPacket packets[PERIODIC_ICB_PACKET_LIMIT];
        int tracking;
    } current;
} executor;

static uint64_t mix64(uint64_t value) {
    value ^= value >> 30; value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27; value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static int command_matches(const PeriodicFormCommand *expected, uint32_t op,
                           const uint64_t fields[8]) {
    if (expected->op != op) return 0;
    for (unsigned field = 0; field < 8; ++field)
        if ((expected->static_mask & (1u << field)) && expected->fields[field] != fields[field]) return 0;
    return 1;
}

static void encode_draw(id<MTL4RenderCommandEncoder> encoder, const ExecutorPacket *packet) {
    const uint64_t *u = packet->fields;
    switch (packet->op) {
        case PERIODIC_METAL_DRAW:
            [encoder drawPrimitives:(MTLPrimitiveType)u[0] vertexStart:(NSUInteger)u[1]
                        vertexCount:(NSUInteger)u[2]];
            break;
        case PERIODIC_METAL_INSTANCED:
            [encoder drawPrimitives:(MTLPrimitiveType)u[0] vertexStart:(NSUInteger)u[1]
                        vertexCount:(NSUInteger)u[2] instanceCount:(NSUInteger)u[3]
                       baseInstance:(NSUInteger)u[4]];
            break;
        case PERIODIC_METAL_INDEXED:
            [encoder drawIndexedPrimitives:(MTLPrimitiveType)u[0] indexCount:(NSUInteger)u[1]
                                 indexType:(MTLIndexType)u[2] indexBuffer:u[3]
                         indexBufferLength:(NSUInteger)u[4] instanceCount:(NSUInteger)u[5]
                                baseVertex:(NSInteger)u[6] baseInstance:(NSUInteger)u[7]];
            break;
        default: break;
    }
}

static uint64_t cache_key(uint64_t form_id, uint32_t cluster_index,
                          const ExecutorPacket *packets, uint32_t count,
                          uint64_t generation, int uses_index_buffer) {
    uint64_t key = mix64(form_id) ^ mix64((uint64_t)cluster_index + 1);
    for (uint32_t packet = 0; packet < count; ++packet) {
        key ^= mix64(packets[packet].op + (uint64_t)packet);
        for (unsigned field = 0; field < 8; ++field)
            key ^= mix64(packets[packet].fields[field] + ((uint64_t)packet << 32) + field);
    }
    if (uses_index_buffer) key ^= mix64(generation);
    return key ? key : 1;
}

static int packets_equal(const ExecutorCache *cache, const ExecutorPacket *packets,
                         uint32_t count, uint64_t generation, uint64_t key) {
    if (cache->key != key || cache->form_id != executor.current.form_id ||
        cache->cluster_index != executor.current.cluster_index ||
        cache->packet_count != count) return 0;
    if (cache->uses_index_buffer && cache->buffer_generation != generation) return 0;
    return !memcmp(cache->packets, packets, (size_t)count * sizeof *packets);
}

static void finish_cluster(void) {
    uint32_t count = executor.current.packet_count;
    if (!count) return;
    int uses_index_buffer = 0;
    for (uint32_t packet = 0; packet < count; ++packet)
        uses_index_buffer |= executor.current.packets[packet].op == PERIODIC_METAL_INDEXED;
    uint64_t generation = executor.buffer_generation ? executor.buffer_generation() : 0;
    uint64_t key = cache_key(executor.current.form_id, executor.current.cluster_index,
                             executor.current.packets, count, generation, uses_index_buffer);
    unsigned first = (unsigned)(mix64(key) & (EXECUTOR_CACHE_SLOTS - 1));
    for (unsigned way = 0; way < EXECUTOR_CACHE_WAYS; ++way) {
        ExecutorCache *cache = &executor.cache[(first + way) & (EXECUTOR_CACHE_SLOTS - 1)];
        if (atomic_load_explicit(&cache->state, memory_order_acquire) == CACHE_READY &&
            packets_equal(cache, executor.current.packets, count, generation, key)) {
            [executor.current.encoder executeCommandsInBuffer:cache->commands
                                                     withRange:NSMakeRange(0, cache->draw_count)];
            atomic_fetch_add_explicit(&executor.executed_clusters, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&executor.executed_draws, cache->draw_count, memory_order_relaxed);
            executor.current.packet_count = 0;
            return;
        }
    }
    for (uint32_t packet = 0; packet < count; ++packet)
        encode_draw(executor.current.encoder, &executor.current.packets[packet]);
    atomic_fetch_add_explicit(&executor.fallback_clusters, 1, memory_order_relaxed);
    for (unsigned way = 0; way < EXECUTOR_CACHE_WAYS; ++way) {
        ExecutorCache *cache = &executor.cache[(first + way) & (EXECUTOR_CACHE_SLOTS - 1)];
        uint32_t expected = atomic_load_explicit(&cache->state, memory_order_relaxed);
        if (expected != CACHE_EMPTY && expected != CACHE_FAILED) continue;
        if (!atomic_compare_exchange_strong_explicit(&cache->state, &expected, CACHE_WRITING,
                                                     memory_order_acq_rel, memory_order_relaxed)) continue;
        cache->key = key;
        cache->form_id = executor.current.form_id;
        cache->cluster_index = executor.current.cluster_index;
        cache->packet_count = count;
        cache->draw_count = count;
        cache->buffer_generation = generation;
        cache->uses_index_buffer = uses_index_buffer;
        memcpy(cache->packets, executor.current.packets, (size_t)count * sizeof *cache->packets);
        atomic_store_explicit(&cache->state, CACHE_COMPILING, memory_order_release);
        atomic_fetch_add_explicit(&executor.queued_clusters, 1, memory_order_relaxed);
        break;
    }
    executor.current.packet_count = 0;
}

static int build_cache(ExecutorCache *cache) {
    MTLIndirectCommandBufferDescriptor *descriptor = [MTLIndirectCommandBufferDescriptor new];
    descriptor.commandTypes = MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed;
    descriptor.inheritPipelineState = YES;
    descriptor.inheritBuffers = YES;
    descriptor.inheritDepthStencilState = YES;
    descriptor.inheritDepthBias = YES;
    descriptor.inheritDepthClipMode = YES;
    descriptor.inheritCullMode = YES;
    descriptor.inheritFrontFacingWinding = YES;
    descriptor.inheritTriangleFillMode = YES;
    id<MTLIndirectCommandBuffer> commands = [executor.device
        newIndirectCommandBufferWithDescriptor:descriptor maxCommandCount:cache->draw_count
        options:MTLResourceStorageModeShared];
    if (!commands) return 0;
    for (uint32_t index = 0; index < cache->packet_count; ++index) {
        const ExecutorPacket *packet = &cache->packets[index];
        const uint64_t *u = packet->fields;
        id<MTLIndirectRenderCommand> command = [commands indirectRenderCommandAtIndex:index];
        switch (packet->op) {
            case PERIODIC_METAL_DRAW:
                [command drawPrimitives:(MTLPrimitiveType)u[0] vertexStart:(NSUInteger)u[1]
                            vertexCount:(NSUInteger)u[2] instanceCount:1 baseInstance:0];
                break;
            case PERIODIC_METAL_INSTANCED:
                [command drawPrimitives:(MTLPrimitiveType)u[0] vertexStart:(NSUInteger)u[1]
                            vertexCount:(NSUInteger)u[2] instanceCount:(NSUInteger)u[3]
                           baseInstance:(NSUInteger)u[4]];
                break;
            case PERIODIC_METAL_INDEXED: {
                if (!executor.resolve_buffer) return 0;
                void *retained = NULL; uint64_t offset = 0;
                if (!executor.resolve_buffer(u[3], u[4], &retained, &offset) || !retained) return 0;
                id<MTLBuffer> buffer = CFBridgingRelease(retained);
                [command drawIndexedPrimitives:(MTLPrimitiveType)u[0] indexCount:(NSUInteger)u[1]
                                     indexType:(MTLIndexType)u[2] indexBuffer:buffer
                             indexBufferOffset:(NSUInteger)offset instanceCount:(NSUInteger)u[5]
                                    baseVertex:(NSInteger)u[6] baseInstance:(NSUInteger)u[7]];
                break; }
            default: return 0;
        }
    }
    if (executor.make_resident) executor.make_resident((__bridge void *)commands);
    cache->commands = commands;
    return 1;
}

static void *compile_clusters(void *unused) {
    (void)unused;
    pthread_setname_np("TOTK periodic ICB");
    for (;;) @autoreleasepool {
        int found = 0;
        for (unsigned slot = 0; slot < EXECUTOR_CACHE_SLOTS; ++slot) {
            ExecutorCache *cache = &executor.cache[slot];
            if (atomic_load_explicit(&cache->state, memory_order_acquire) != CACHE_COMPILING) continue;
            found = 1;
            int built = build_cache(cache);
            if (built) atomic_fetch_add_explicit(&executor.compiled_clusters, 1, memory_order_relaxed);
            atomic_store_explicit(&cache->state, built ? CACHE_READY : CACHE_FAILED, memory_order_release);
        }
        if (atomic_load_explicit(&executor.stopping, memory_order_relaxed)) break;
        if (!found) { struct timespec pause = {.tv_nsec=1000000}; nanosleep(&pause, NULL); }
    }
    return NULL;
}

void periodic_executor_init(id<MTLDevice> device,
                            PeriodicBufferGenerationFn buffer_generation,
                            PeriodicResolveBufferFn resolve_buffer,
                            PeriodicMakeResidentFn make_resident) {
    const char *enabled = getenv("TOTK_PERIODIC_ICB");
    if (periodic_executor_active() || !device || (enabled && !strcmp(enabled, "0"))) return;
    executor.device = device;
    executor.buffer_generation = buffer_generation;
    executor.resolve_buffer = resolve_buffer;
    executor.make_resident = make_resident;
    executor.cache = calloc(EXECUTOR_CACHE_SLOTS, sizeof *executor.cache);
    if (!executor.cache || pthread_create(&executor.compiler, NULL, compile_clusters, NULL)) {
        free(executor.cache); executor.cache = NULL; executor.device = nil; return;
    }
    atomic_store_explicit(&executor.active, 1, memory_order_release);
    atexit(periodic_executor_finish);
}

int periodic_executor_active(void) { return atomic_load_explicit(&executor.active, memory_order_relaxed); }

int periodic_executor_begin(uint64_t form_id, id<MTL4RenderCommandEncoder> encoder,
                            const uint64_t fields[8]) {
    memset(&executor.current, 0, sizeof executor.current);
    if (!periodic_executor_active() || !form_id || !encoder ||
        !periodic_form_snapshot(form_id, &executor.current.form) ||
        !(executor.current.program = periodic_compiler_form(form_id)) ||
        !executor.current.form.command_count ||
        !command_matches(&executor.current.form.commands[0], PERIODIC_METAL_BEGIN, fields)) return 0;
    executor.current.encoder = encoder;
    executor.current.form_id = form_id;
    executor.current.command_index = 1;
    executor.current.tracking = 1;
    atomic_fetch_add_explicit(&executor.tracked_passes, 1, memory_order_relaxed);
    return 1;
}

int periodic_executor_packet(uint32_t op, const uint64_t fields[8]) {
    if (!executor.current.tracking) return 0;
    uint32_t command_index = executor.current.command_index;
    if (command_index >= executor.current.form.command_count ||
        !command_matches(&executor.current.form.commands[command_index], op, fields)) {
        finish_cluster();
        executor.current.tracking = 0;
        atomic_fetch_add_explicit(&executor.rejected_passes, 1, memory_order_relaxed);
        return 0;
    }
    while (executor.current.cluster_index < executor.current.program->cluster_count) {
        const PeriodicICBCluster *cluster = &executor.current.program->clusters[executor.current.cluster_index];
        if (command_index < cluster->first_command + cluster->command_count) break;
        executor.current.cluster_index++;
    }
    const PeriodicICBCluster *cluster = executor.current.cluster_index < executor.current.program->cluster_count
        ? &executor.current.program->clusters[executor.current.cluster_index] : NULL;
    int handled = cluster && cluster->icb_eligible && command_index >= cluster->first_command &&
                  command_index < cluster->first_command + cluster->command_count;
    if (handled) {
        if (executor.current.packet_count == PERIODIC_ICB_PACKET_LIMIT) {
            finish_cluster(); executor.current.tracking = 0;
            atomic_fetch_add_explicit(&executor.rejected_passes, 1, memory_order_relaxed);
            return 0;
        }
        ExecutorPacket *packet = &executor.current.packets[executor.current.packet_count++];
        packet->op = op; memcpy(packet->fields, fields, sizeof packet->fields);
    }
    executor.current.command_index++;
    if (handled && executor.current.command_index == cluster->first_command + cluster->command_count) {
        finish_cluster();
        executor.current.cluster_index++;
    }
    if (op == PERIODIC_METAL_END) {
        if (executor.current.command_index != executor.current.form.command_count) {
            executor.current.tracking = 0;
            atomic_fetch_add_explicit(&executor.rejected_passes, 1, memory_order_relaxed);
        } else executor.current.tracking = 0;
    }
    return handled;
}

void periodic_executor_flush(void) {
    if (!periodic_executor_active()) return;
    for (;;) {
        int pending = 0;
        for (unsigned slot = 0; slot < EXECUTOR_CACHE_SLOTS; ++slot) {
            uint32_t state = atomic_load_explicit(&executor.cache[slot].state, memory_order_acquire);
            pending |= state == CACHE_WRITING || state == CACHE_COMPILING;
        }
        if (!pending) return;
        struct timespec pause = {.tv_nsec=100000}; nanosleep(&pause, NULL);
    }
}

PeriodicExecutorStats periodic_executor_stats(void) {
    return (PeriodicExecutorStats) {
        .tracked_passes=atomic_load_explicit(&executor.tracked_passes, memory_order_relaxed),
        .rejected_passes=atomic_load_explicit(&executor.rejected_passes, memory_order_relaxed),
        .queued_clusters=atomic_load_explicit(&executor.queued_clusters, memory_order_relaxed),
        .compiled_clusters=atomic_load_explicit(&executor.compiled_clusters, memory_order_relaxed),
        .executed_clusters=atomic_load_explicit(&executor.executed_clusters, memory_order_relaxed),
        .fallback_clusters=atomic_load_explicit(&executor.fallback_clusters, memory_order_relaxed),
        .executed_draws=atomic_load_explicit(&executor.executed_draws, memory_order_relaxed),
    };
}

void periodic_executor_finish(void) {
    if (!periodic_executor_active()) return;
    periodic_executor_flush();
    if (!atomic_exchange_explicit(&executor.active, 0, memory_order_acq_rel)) return;
    atomic_store_explicit(&executor.stopping, 1, memory_order_release);
    pthread_join(executor.compiler, NULL);
    PeriodicExecutorStats stats = periodic_executor_stats();
    if (stats.tracked_passes)
        fprintf(stderr, "[totk] periodic executor: %llu tracked passes, %llu ICB clusters / %llu draws executed, %llu fallbacks, %llu rejected\n",
                (unsigned long long)stats.tracked_passes, (unsigned long long)stats.executed_clusters,
                (unsigned long long)stats.executed_draws, (unsigned long long)stats.fallback_clusters,
                (unsigned long long)stats.rejected_passes);
    for (unsigned slot = 0; slot < EXECUTOR_CACHE_SLOTS; ++slot) executor.cache[slot].commands = nil;
    free(executor.cache); executor.cache = NULL; executor.device = nil;
}

static __strong id<MTLResidencySet> probe_residency;
static __strong id<MTLBuffer> probe_index_buffer;
static void probe_make_resident(void *allocation) {
    [probe_residency addAllocation:(__bridge id<MTLAllocation>)allocation];
}
static uint64_t probe_buffer_generation(void) { return 1; }
static int probe_resolve_buffer(uint64_t address, uint64_t length,
                                void **retained, uint64_t *offset) {
    uint64_t base = probe_index_buffer.gpuAddress;
    uint64_t end = address + length;
    if (!probe_index_buffer || !retained || !offset || end < address ||
        address < base || end > base + probe_index_buffer.length) return 0;
    *retained = (void *)CFBridgingRetain(probe_index_buffer);
    *offset = address - base;
    return 1;
}

int periodic_executor_probe(void) {
    setenv("TOTK_PERIODIC_FORMS", "1", 1);
    periodic_compiler_init();
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    const uint16_t indices[] = {0, 1, 2};
    probe_index_buffer = [device newBufferWithBytes:indices length:sizeof indices
                                            options:MTLResourceStorageModeShared];
    const uint64_t begin[8] = {0};
    const uint64_t draw_a[8] = {MTLPrimitiveTypeTriangle, 3, MTLIndexTypeUInt16,
        probe_index_buffer.gpuAddress, probe_index_buffer.length, 1, 0, 0};
    const uint64_t draw_b[8] = {MTLPrimitiveTypeTriangle, 3, MTLIndexTypeUInt16,
        probe_index_buffer.gpuAddress, probe_index_buffer.length, 1, 0, 0};
    const uint64_t draw_a_variant[8] = {MTLPrimitiveTypeTriangle, 3, MTLIndexTypeUInt16,
        probe_index_buffer.gpuAddress, probe_index_buffer.length, 1, 0, 1};
    const uint64_t draw_b_variant[8] = {MTLPrimitiveTypeTriangle, 3, MTLIndexTypeUInt16,
        probe_index_buffer.gpuAddress, probe_index_buffer.length, 1, 0, 1};
    const uint64_t end[8] = {0};
    for (uint64_t anchor = 0; anchor < 3; ++anchor) {
        periodic_observe_packet(PERIODIC_METAL_BEGIN, begin, 0x30, 0, 0x0f,
            PERIODIC_PACKET_PASS_BEGIN | PERIODIC_PACKET_ICB_BREAK, NULL, 8000 + anchor);
        periodic_observe_packet(PERIODIC_METAL_INDEXED, draw_a, 0x05, 0xf2, 0x08,
            PERIODIC_PACKET_DRAW | PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
        periodic_observe_packet(PERIODIC_METAL_INDEXED, draw_b, 0x05, 0xf2, 0x08,
            PERIODIC_PACKET_DRAW | PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
        periodic_observe_packet(PERIODIC_METAL_END, end, 0x03, 0, 0,
            PERIODIC_PACKET_PASS_END | PERIODIC_PACKET_ICB_BREAK, NULL, 0);
        periodic_anchor_publish(anchor);
    }
    periodic_form_flush(); periodic_compiler_flush();
    uint64_t form_id = 0;
    if (!periodic_form_expected(0, &form_id, NULL) || form_id != 1) return 1;
    NSError *error = nil;
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i [[vertex_id]]) { const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1);}\n"
        "fragment float4 f(){return float4(0.125,0.375,0.625,1);}\n";
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [library newFunctionWithName:@"v"]; pd.fragmentFunction = [library newFunctionWithName:@"f"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; pd.supportIndirectCommandBuffers = YES;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:td];
    MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new]; rd.initialCapacity = 4;
    probe_residency = [device newResidencySetWithDescriptor:rd error:&error];
    periodic_executor_init(device, probe_buffer_generation, probe_resolve_buffer, probe_make_resident);
    if (!device || !pipeline || !texture || !probe_residency || !probe_index_buffer ||
        !periodic_executor_active()) return 1;
    [probe_residency addAllocation:(id<MTLAllocation>)pipeline];
    [probe_residency addAllocation:(id<MTLAllocation>)texture];
    [probe_residency addAllocation:(id<MTLAllocation>)probe_index_buffer];
    [probe_residency commit];
    id<MTL4CommandQueue> queue = [device newMTL4CommandQueue];
    id<MTL4CommandAllocator> allocator = [device newCommandAllocatorWithDescriptor:[MTL4CommandAllocatorDescriptor new] error:&error];
    id<MTL4CommandBuffer> command_buffer = [device newCommandBuffer];
    [command_buffer beginCommandBufferWithAllocator:allocator]; [command_buffer useResidencySet:probe_residency];
    for (unsigned pass = 0; pass < 4; ++pass) {
        const uint64_t *first_draw = pass < 2 ? draw_a : draw_a_variant;
        const uint64_t *second_draw = pass < 2 ? draw_b : draw_b_variant;
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        rp.colorAttachments[0].texture = texture; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        id<MTL4RenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:rp];
        [encoder setRenderPipelineState:pipeline];
        if (!periodic_executor_begin(form_id, encoder, begin) ||
            !periodic_executor_packet(PERIODIC_METAL_INDEXED, first_draw) ||
            !periodic_executor_packet(PERIODIC_METAL_INDEXED, second_draw)) return 1;
        periodic_executor_packet(PERIODIC_METAL_END, end);
        [encoder endEncoding];
        if (pass == 0 || pass == 2) periodic_executor_flush();
    }
    [probe_residency commit]; [command_buffer endCommandBuffer];
    id<MTL4CommandBuffer> buffers[] = {command_buffer};
    dispatch_semaphore_t done = dispatch_semaphore_create(0); __block NSError *gpu_error = nil;
    MTL4CommitOptions *options = [MTL4CommitOptions new];
    [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) { gpu_error = feedback.error; dispatch_semaphore_signal(done); }];
    [queue commit:buffers count:1 options:options];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) || gpu_error) return 1;
    uint8_t pixels[64] = {0}; [texture getBytes:pixels bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
    const uint8_t *center = pixels + 40;
    PeriodicExecutorStats stats = periodic_executor_stats();
    if (center[0] < 30 || center[0] > 34 || center[1] < 94 || center[1] > 98 ||
        center[2] < 158 || center[2] > 162 || center[3] != 255 ||
        stats.compiled_clusters != 2 || stats.executed_clusters != 2 || stats.executed_draws != 4 ||
        stats.fallback_clusters != 2 || stats.rejected_passes) return 1;
    printf("{\"probe\":\"periodic-executor\",\"status\":\"pass\",\"indexed\":true,\"cache_variants\":2,\"compiled_clusters\":2,\"executed_clusters\":2,\"executed_draws\":4,\"hot_path_allocations\":0}\n");
    return 0;
}
