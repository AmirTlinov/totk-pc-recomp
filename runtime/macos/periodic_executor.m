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

enum { EXECUTOR_CACHE_SLOTS = 4096, EXECUTOR_CACHE_WAYS = 4,
       EXECUTOR_PACKET_LIMIT = PERIODIC_ICB_PACKET_LIMIT + 8 };
enum { CACHE_EMPTY, CACHE_COMPILING, CACHE_READY, CACHE_FAILED, CACHE_WRITING };
enum { STATE_PSO, STATE_DSS, STATE_CULL, STATE_WINDING, STATE_FILL, STATE_BIAS,
       STATE_CLIP, STATE_COUNT };

typedef struct {
    uint32_t op;
    uint32_t reserved;
    uint64_t fields[8];
} ExecutorPacket;

typedef struct {
    _Atomic uint32_t state;
    uint64_t key;
    uint64_t form_id;
    uint32_t cluster_index;
    uint32_t packet_count;
    uint32_t draw_count;
    uint32_t uses_index_buffer;
    PeriodicBufferLifetime *buffer_lifetimes[EXECUTOR_PACKET_LIMIT];
    uint64_t buffer_generations[EXECUTOR_PACKET_LIMIT];
    ExecutorPacket packets[EXECUTOR_PACKET_LIMIT];
    __strong id<MTLIndirectCommandBuffer> commands;
} ExecutorCache;

static struct {
    __strong id<MTLDevice> device;
    PeriodicResolveBufferFn resolve_buffer;
    PeriodicMakeResidentFn make_resident;
    ExecutorCache *cache;
    uint32_t *compile_slots;
    _Alignas(64) _Atomic uint64_t compile_read;
    _Alignas(64) _Atomic uint64_t compile_write;
    uint64_t producer_write, producer_read_cache;
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
        ExecutorPacket packets[EXECUTOR_PACKET_LIMIT];
        ExecutorPacket states[STATE_COUNT];
        uint8_t state_valid[STATE_COUNT];
        int tracking;
    } current;
} executor;

static int queue_compile(uint32_t slot) {
    uint64_t write = executor.producer_write;
    uint64_t read = executor.producer_read_cache;
    if (write - read == EXECUTOR_CACHE_SLOTS) {
        read = atomic_load_explicit(&executor.compile_read, memory_order_acquire);
        executor.producer_read_cache = read;
    }
    if (write - read == EXECUTOR_CACHE_SLOTS) return 0;
    executor.compile_slots[write & (EXECUTOR_CACHE_SLOTS - 1)] = slot;
    executor.producer_write = write + 1;
    atomic_store_explicit(&executor.compile_write, write + 1, memory_order_release);
    return 1;
}

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

static int state_slot(uint32_t op) {
    switch (op) {
        case PERIODIC_METAL_PSO: return STATE_PSO;
        case PERIODIC_METAL_DSS: return STATE_DSS;
        case PERIODIC_METAL_CULL: return STATE_CULL;
        case PERIODIC_METAL_WINDING: return STATE_WINDING;
        case PERIODIC_METAL_FILL: return STATE_FILL;
        case PERIODIC_METAL_BIAS: return STATE_BIAS;
        case PERIODIC_METAL_CLIP: return STATE_CLIP;
        default: return -1;
    }
}

static void release_packet_object(const ExecutorPacket *packet) {
    if ((packet->op == PERIODIC_METAL_PSO || packet->op == PERIODIC_METAL_DSS) &&
        packet->fields[0]) CFRelease((CFTypeRef)(uintptr_t)packet->fields[0]);
}

static void retain_packet_object(const ExecutorPacket *packet) {
    if ((packet->op == PERIODIC_METAL_PSO || packet->op == PERIODIC_METAL_DSS) &&
        packet->fields[0]) CFRetain((CFTypeRef)(uintptr_t)packet->fields[0]);
}

static void encode_state(id<MTL4RenderCommandEncoder> encoder, const ExecutorPacket *packet) {
    const uint64_t *u = packet->fields;
    switch (packet->op) {
        case PERIODIC_METAL_PSO:
            [encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)(void *)(uintptr_t)u[0]];
            break;
        case PERIODIC_METAL_DSS:
            [encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)(void *)(uintptr_t)u[0]];
            break;
        case PERIODIC_METAL_CULL: [encoder setCullMode:(MTLCullMode)u[0]]; break;
        case PERIODIC_METAL_WINDING: [encoder setFrontFacingWinding:(MTLWinding)u[0]]; break;
        case PERIODIC_METAL_FILL: [encoder setTriangleFillMode:(MTLTriangleFillMode)u[0]]; break;
        case PERIODIC_METAL_BIAS: {
            float value[3]; memcpy(value, u, sizeof value);
            [encoder setDepthBias:value[0] slopeScale:value[1] clamp:value[2]];
            break;
        }
        case PERIODIC_METAL_CLIP: [encoder setDepthClipMode:(MTLDepthClipMode)u[0]]; break;
        default: break;
    }
}

static void encode_indirect_state(id<MTLIndirectRenderCommand> command,
                                  const ExecutorPacket *packet) {
    const uint64_t *u = packet->fields;
    switch (packet->op) {
        case PERIODIC_METAL_PSO:
            [command setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)(void *)(uintptr_t)u[0]];
            break;
        case PERIODIC_METAL_DSS:
            [command setDepthStencilState:(__bridge id<MTLDepthStencilState>)(void *)(uintptr_t)u[0]];
            break;
        case PERIODIC_METAL_CULL: [command setCullMode:(MTLCullMode)u[0]]; break;
        case PERIODIC_METAL_WINDING: [command setFrontFacingWinding:(MTLWinding)u[0]]; break;
        case PERIODIC_METAL_FILL: [command setTriangleFillMode:(MTLTriangleFillMode)u[0]]; break;
        case PERIODIC_METAL_BIAS: {
            float value[3]; memcpy(value, u, sizeof value);
            [command setDepthBias:value[0] slopeScale:value[1] clamp:value[2]];
            break;
        }
        case PERIODIC_METAL_CLIP: [command setDepthClipMode:(MTLDepthClipMode)u[0]]; break;
        default: break;
    }
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

static void capture_state(uint32_t op, const uint64_t fields[8]) {
    unsigned slot = (unsigned)state_slot(op);
    if (executor.current.state_valid[slot]) release_packet_object(&executor.current.states[slot]);
    executor.current.states[slot].op = op;
    memcpy(executor.current.states[slot].fields, fields, sizeof executor.current.states[slot].fields);
    retain_packet_object(&executor.current.states[slot]);
    executor.current.state_valid[slot] = 1;
}

static void restore_state(void) {
    for (unsigned slot = 0; slot < STATE_COUNT; ++slot)
        if (executor.current.state_valid[slot])
            encode_state(executor.current.encoder, &executor.current.states[slot]);
}

static void release_cluster_packets(void) {
    for (uint32_t packet = 0; packet < executor.current.packet_count; ++packet)
        release_packet_object(&executor.current.packets[packet]);
    executor.current.packet_count = 0;
}

static void reset_current(void) {
    release_cluster_packets();
    for (unsigned slot = 0; slot < STATE_COUNT; ++slot)
        if (executor.current.state_valid[slot]) release_packet_object(&executor.current.states[slot]);
    memset(&executor.current, 0, sizeof executor.current);
}

static int begin_cluster_packets(void) {
    if (executor.current.packet_count) return 1;
    for (unsigned slot = 0; slot < STATE_COUNT; ++slot) {
        if (!executor.current.state_valid[slot]) continue;
        if (executor.current.packet_count == EXECUTOR_PACKET_LIMIT) return 0;
        ExecutorPacket *packet = &executor.current.packets[executor.current.packet_count++];
        *packet = executor.current.states[slot];
        retain_packet_object(packet);
    }
    return 1;
}

static uint64_t cache_key(uint64_t form_id, uint32_t cluster_index,
                          const ExecutorPacket *packets, uint32_t count) {
    uint64_t key = mix64(form_id) ^ mix64((uint64_t)cluster_index + 1);
    for (uint32_t packet = 0; packet < count; ++packet) {
        key ^= mix64(packets[packet].op + (uint64_t)packet);
        for (unsigned field = 0; field < 8; ++field)
            key ^= mix64(packets[packet].fields[field] + ((uint64_t)packet << 32) + field);
    }
    return key ? key : 1;
}

static int packets_equal(const ExecutorCache *cache, const ExecutorPacket *packets,
                         uint32_t count, uint64_t key) {
    if (cache->key != key || cache->form_id != executor.current.form_id ||
        cache->cluster_index != executor.current.cluster_index ||
        cache->packet_count != count) return 0;
    if (cache->uses_index_buffer)
        for (uint32_t packet = 0; packet < count; ++packet) {
            if (cache->packets[packet].op != PERIODIC_METAL_INDEXED) continue;
            if (!cache->buffer_lifetimes[packet] ||
                atomic_load_explicit(&cache->buffer_lifetimes[packet]->generation,
                                     memory_order_acquire) != cache->buffer_generations[packet])
                return 0;
        }
    return !memcmp(cache->packets, packets, (size_t)count * sizeof *packets);
}

static void finish_cluster(void) {
    uint32_t count = executor.current.packet_count;
    if (!count) return;
    int uses_index_buffer = 0; uint32_t draws = 0;
    for (uint32_t packet = 0; packet < count; ++packet) {
        uses_index_buffer |= executor.current.packets[packet].op == PERIODIC_METAL_INDEXED;
        draws += executor.current.packets[packet].op == PERIODIC_METAL_DRAW ||
                 executor.current.packets[packet].op == PERIODIC_METAL_INSTANCED ||
                 executor.current.packets[packet].op == PERIODIC_METAL_INDEXED;
    }
    if (!draws) { release_cluster_packets(); return; }
    uint64_t key = cache_key(executor.current.form_id, executor.current.cluster_index,
                             executor.current.packets, count);
    unsigned first = (unsigned)(mix64(key) & (EXECUTOR_CACHE_SLOTS - 1));
    for (unsigned way = 0; way < EXECUTOR_CACHE_WAYS; ++way) {
        ExecutorCache *cache = &executor.cache[(first + way) & (EXECUTOR_CACHE_SLOTS - 1)];
        if (atomic_load_explicit(&cache->state, memory_order_acquire) == CACHE_READY &&
            packets_equal(cache, executor.current.packets, count, key)) {
            [executor.current.encoder executeCommandsInBuffer:cache->commands
                                                     withRange:NSMakeRange(0, cache->draw_count)];
            atomic_fetch_add_explicit(&executor.executed_clusters, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&executor.executed_draws, cache->draw_count, memory_order_relaxed);
            restore_state();
            release_cluster_packets();
            return;
        }
    }
    for (uint32_t packet = 0; packet < count; ++packet) {
        if (state_slot(executor.current.packets[packet].op) >= 0)
            encode_state(executor.current.encoder, &executor.current.packets[packet]);
        else
            encode_draw(executor.current.encoder, &executor.current.packets[packet]);
    }
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
        cache->draw_count = draws;
        cache->uses_index_buffer = uses_index_buffer;
        memset(cache->buffer_lifetimes, 0, sizeof cache->buffer_lifetimes);
        memset(cache->buffer_generations, 0, sizeof cache->buffer_generations);
        memcpy(cache->packets, executor.current.packets, (size_t)count * sizeof *cache->packets);
        for (uint32_t packet = 0; packet < count; ++packet)
            retain_packet_object(&cache->packets[packet]);
        atomic_store_explicit(&cache->state, CACHE_COMPILING, memory_order_release);
        if (queue_compile((uint32_t)(cache - executor.cache)))
            atomic_fetch_add_explicit(&executor.queued_clusters, 1, memory_order_relaxed);
        else {
            for (uint32_t packet = 0; packet < count; ++packet)
                release_packet_object(&cache->packets[packet]);
            atomic_store_explicit(&cache->state, CACHE_FAILED, memory_order_release);
        }
        break;
    }
    release_cluster_packets();
}

static int build_cache(ExecutorCache *cache) {
    MTLIndirectCommandBufferDescriptor *descriptor = [MTLIndirectCommandBufferDescriptor new];
    int indexed = 0; uint32_t state_mask = 0;
    for (uint32_t index = 0; index < cache->packet_count; ++index) {
        indexed |= cache->packets[index].op == PERIODIC_METAL_INDEXED;
        int slot = state_slot(cache->packets[index].op);
        if (slot >= 0) state_mask |= 1u << slot;
    }
    descriptor.commandTypes = indexed
        ? MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed
        : MTLIndirectCommandTypeDraw;
    descriptor.inheritPipelineState = !(state_mask & (1u << STATE_PSO));
    descriptor.inheritBuffers = YES;
    descriptor.inheritDepthStencilState = !(state_mask & (1u << STATE_DSS));
    descriptor.inheritDepthBias = !(state_mask & (1u << STATE_BIAS));
    descriptor.inheritDepthClipMode = !(state_mask & (1u << STATE_CLIP));
    descriptor.inheritCullMode = !(state_mask & (1u << STATE_CULL));
    descriptor.inheritFrontFacingWinding = !(state_mask & (1u << STATE_WINDING));
    descriptor.inheritTriangleFillMode = !(state_mask & (1u << STATE_FILL));
    id<MTLIndirectCommandBuffer> commands = [executor.device
        newIndirectCommandBufferWithDescriptor:descriptor maxCommandCount:cache->draw_count
        options:MTLResourceStorageModeShared];
    if (!commands) return 0;
    const ExecutorPacket *states[STATE_COUNT] = {0};
    uint32_t draw_index = 0;
    for (uint32_t index = 0; index < cache->packet_count; ++index) {
        const ExecutorPacket *packet = &cache->packets[index];
        const uint64_t *u = packet->fields;
        int slot = state_slot(packet->op);
        if (slot >= 0) {
            states[slot] = packet;
            if (packet->op == PERIODIC_METAL_PSO && executor.make_resident)
                executor.make_resident((void *)(uintptr_t)u[0]);
            continue;
        }
        id<MTLIndirectRenderCommand> command = [commands indirectRenderCommandAtIndex:draw_index++];
        for (unsigned state = 0; state < STATE_COUNT; ++state)
            if (states[state]) encode_indirect_state(command, states[state]);
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
                void *retained = NULL; uint64_t offset = 0, generation = 0;
                PeriodicBufferLifetime *lifetime = NULL;
                if (!executor.resolve_buffer(u[3], u[4], &retained, &offset,
                                             &lifetime, &generation) ||
                    !retained || !lifetime || !generation) return 0;
                id<MTLBuffer> buffer = CFBridgingRelease(retained);
                cache->buffer_lifetimes[index] = lifetime;
                cache->buffer_generations[index] = generation;
                [command drawIndexedPrimitives:(MTLPrimitiveType)u[0] indexCount:(NSUInteger)u[1]
                                     indexType:(MTLIndexType)u[2] indexBuffer:buffer
                             indexBufferOffset:(NSUInteger)offset instanceCount:(NSUInteger)u[5]
                                    baseVertex:(NSInteger)u[6] baseInstance:(NSUInteger)u[7]];
                break; }
            default: return 0;
        }
    }
    if (draw_index != cache->draw_count) return 0;
    if (executor.make_resident) executor.make_resident((__bridge void *)commands);
    cache->commands = commands;
    return 1;
}

static void *compile_clusters(void *unused) {
    (void)unused;
    pthread_setname_np("TOTK periodic ICB");
    for (;;) @autoreleasepool {
        uint64_t read = atomic_load_explicit(&executor.compile_read, memory_order_relaxed);
        uint64_t write = atomic_load_explicit(&executor.compile_write, memory_order_acquire);
        if (read == write) {
            if (atomic_load_explicit(&executor.stopping, memory_order_relaxed)) break;
            struct timespec pause = {.tv_nsec=1000000}; nanosleep(&pause, NULL);
            continue;
        }
        ExecutorCache *cache = &executor.cache[executor.compile_slots[read & (EXECUTOR_CACHE_SLOTS - 1)]];
        int built = atomic_load_explicit(&cache->state, memory_order_acquire) == CACHE_COMPILING &&
                    build_cache(cache);
        for (uint32_t packet = 0; packet < cache->packet_count; ++packet)
            release_packet_object(&cache->packets[packet]);
        if (built) atomic_fetch_add_explicit(&executor.compiled_clusters, 1, memory_order_relaxed);
        atomic_store_explicit(&cache->state, built ? CACHE_READY : CACHE_FAILED, memory_order_release);
        atomic_store_explicit(&executor.compile_read, read + 1, memory_order_release);
    }
    return NULL;
}

void periodic_executor_init(id<MTLDevice> device,
                            PeriodicResolveBufferFn resolve_buffer,
                            PeriodicMakeResidentFn make_resident) {
    const char *enabled = getenv("TOTK_PERIODIC_ICB");
    if (periodic_executor_active() || !device || (enabled && !strcmp(enabled, "0"))) return;
    executor.device = device;
    executor.resolve_buffer = resolve_buffer;
    executor.make_resident = make_resident;
    executor.cache = calloc(EXECUTOR_CACHE_SLOTS, sizeof *executor.cache);
    executor.compile_slots = calloc(EXECUTOR_CACHE_SLOTS, sizeof *executor.compile_slots);
    if (!executor.cache || !executor.compile_slots ||
        pthread_create(&executor.compiler, NULL, compile_clusters, NULL)) {
        free(executor.cache); free(executor.compile_slots);
        executor.cache = NULL; executor.compile_slots = NULL; executor.device = nil; return;
    }
    atomic_store_explicit(&executor.active, 1, memory_order_release);
    atexit(periodic_executor_finish);
}

int periodic_executor_active(void) { return atomic_load_explicit(&executor.active, memory_order_relaxed); }

int periodic_executor_begin(uint64_t form_id, id<MTL4RenderCommandEncoder> encoder,
                            const uint64_t fields[8]) {
    reset_current();
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
        reset_current();
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
        if (!begin_cluster_packets() || executor.current.packet_count == EXECUTOR_PACKET_LIMIT) {
            finish_cluster(); executor.current.tracking = 0;
            atomic_fetch_add_explicit(&executor.rejected_passes, 1, memory_order_relaxed);
            return 0;
        }
        ExecutorPacket *packet = &executor.current.packets[executor.current.packet_count++];
        packet->op = op;
        memcpy(packet->fields, fields, sizeof packet->fields);
    }
    int slot = state_slot(op);
    if (slot >= 0) capture_state(op, fields);
    executor.current.command_index++;
    if (handled && executor.current.command_index == cluster->first_command + cluster->command_count) {
        finish_cluster();
        executor.current.cluster_index++;
    }
    if (op == PERIODIC_METAL_END) {
        int complete = executor.current.command_index == executor.current.form.command_count;
        reset_current();
        if (!complete)
            atomic_fetch_add_explicit(&executor.rejected_passes, 1, memory_order_relaxed);
    }
    return handled;
}

void periodic_executor_flush(void) {
    if (!periodic_executor_active()) return;
    while (atomic_load_explicit(&executor.compile_read, memory_order_acquire) !=
           atomic_load_explicit(&executor.compile_write, memory_order_acquire)) {
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

void periodic_executor_report(void) {
    PeriodicExecutorStats stats = periodic_executor_stats();
    if (stats.tracked_passes)
        fprintf(stderr, "[totk] periodic executor: %llu tracked, %llu rejected; %llu queued, %llu compiled; %llu ICB clusters / %llu draws executed, %llu fallbacks\n",
                (unsigned long long)stats.tracked_passes, (unsigned long long)stats.rejected_passes,
                (unsigned long long)stats.queued_clusters, (unsigned long long)stats.compiled_clusters,
                (unsigned long long)stats.executed_clusters, (unsigned long long)stats.executed_draws,
                (unsigned long long)stats.fallback_clusters);
}

void periodic_executor_finish(void) {
    if (!periodic_executor_active()) return;
    periodic_executor_flush();
    reset_current();
    if (!atomic_exchange_explicit(&executor.active, 0, memory_order_acq_rel)) return;
    atomic_store_explicit(&executor.stopping, 1, memory_order_release);
    pthread_join(executor.compiler, NULL);
    periodic_executor_report();
    for (unsigned slot = 0; slot < EXECUTOR_CACHE_SLOTS; ++slot) {
        executor.cache[slot].commands = nil;
    }
    free(executor.cache); free(executor.compile_slots);
    executor.cache = NULL; executor.compile_slots = NULL; executor.device = nil;
}

static __strong id<MTLResidencySet> probe_residency;
static __strong id<MTLBuffer> probe_index_buffer;
static PeriodicBufferLifetime probe_buffer_lifetime = {.generation=1};
static void probe_make_resident(void *allocation) {
    [probe_residency addAllocation:(__bridge id<MTLAllocation>)allocation];
}
static int probe_resolve_buffer(uint64_t address, uint64_t length,
                                void **retained, uint64_t *offset,
                                PeriodicBufferLifetime **lifetime,
                                uint64_t *generation) {
    uint64_t base = probe_index_buffer.gpuAddress;
    uint64_t end = address + length;
    if (!probe_index_buffer || !retained || !offset || !lifetime || !generation || end < address ||
        address < base || end > base + probe_index_buffer.length) return 0;
    *retained = (void *)CFBridgingRetain(probe_index_buffer);
    *offset = address - base;
    *lifetime = &probe_buffer_lifetime;
    *generation = atomic_load_explicit(&probe_buffer_lifetime.generation, memory_order_relaxed);
    return 1;
}

int periodic_executor_probe(void) {
    setenv("TOTK_PERIODIC_FORMS", "1", 1);
    atomic_store_explicit(&probe_buffer_lifetime.generation, 1, memory_order_relaxed);
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
    const uint64_t object_shape[8] = {0};
    const uint64_t cull[8] = {MTLCullModeNone};
    const uint64_t winding[8] = {MTLWindingClockwise};
    const uint64_t fill[8] = {MTLTriangleFillModeFill};
    const uint64_t clip[8] = {MTLDepthClipModeClip};
    uint64_t bias[8] = {0};
    const float bias_values[3] = {0.0f, 0.0f, 0.0f};
    memcpy(bias, bias_values, sizeof bias_values);
    const uint64_t end[8] = {0};
    for (uint64_t anchor = 0; anchor < 2; ++anchor) {
        periodic_observe_packet(PERIODIC_METAL_BEGIN, begin, 0x30, 0, 0x0f,
            PERIODIC_PACKET_PASS_BEGIN | PERIODIC_PACKET_ICB_BREAK, NULL, 8000 + anchor);
        periodic_observe_packet(PERIODIC_METAL_PSO, object_shape, 0, 0, 0x01,
            PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
        periodic_observe_packet(PERIODIC_METAL_DSS, object_shape, 0, 0, 0x01,
            PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
        periodic_observe_packet(PERIODIC_METAL_CULL, cull, 0, 0x01, 0,
            PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
        periodic_observe_packet(PERIODIC_METAL_WINDING, winding, 0, 0x01, 0,
            PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
        periodic_observe_packet(PERIODIC_METAL_FILL, fill, 0, 0x01, 0,
            PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
        periodic_observe_packet(PERIODIC_METAL_BIAS, bias, 0, 0xff, 0,
            PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
        periodic_observe_packet(PERIODIC_METAL_CLIP, clip, 0, 0x01, 0,
            PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
        periodic_observe_packet(PERIODIC_METAL_INDEXED, draw_a, 0x05, 0xf2, 0x08,
            PERIODIC_PACKET_DRAW | PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
        periodic_observe_packet(PERIODIC_METAL_INDEXED, draw_b, 0x05, 0xf2, 0x08,
            PERIODIC_PACKET_DRAW | PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
        periodic_observe_packet(PERIODIC_METAL_END, end, 0x03, 0, 0,
            PERIODIC_PACKET_PASS_END | PERIODIC_PACKET_ICB_BREAK, NULL, 0);
        periodic_anchor_publish(anchor);
        periodic_form_flush();
        if (!anchor) {
            uint64_t template_id = 0;
            if (!periodic_form_template_for_pass(8000, &template_id) || template_id != 1 ||
                !periodic_form_certify(template_id)) return 1;
        }
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
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = NO;
    id<MTLDepthStencilState> depth_stencil = [device newDepthStencilStateWithDescriptor:dd];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:td];
    MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new]; rd.initialCapacity = 4;
    probe_residency = [device newResidencySetWithDescriptor:rd error:&error];
    periodic_executor_init(device, probe_resolve_buffer, probe_make_resident);
    if (!device || !pipeline || !depth_stencil || !texture || !probe_residency || !probe_index_buffer ||
        !periodic_executor_active()) return 1;
    [probe_residency addAllocation:(id<MTLAllocation>)pipeline];
    [probe_residency addAllocation:(id<MTLAllocation>)texture];
    [probe_residency addAllocation:(id<MTLAllocation>)probe_index_buffer];
    [probe_residency commit];
    id<MTL4CommandQueue> queue = [device newMTL4CommandQueue];
    id<MTL4CommandAllocator> allocator = [device newCommandAllocatorWithDescriptor:[MTL4CommandAllocatorDescriptor new] error:&error];
    id<MTL4CommandBuffer> command_buffer = [device newCommandBuffer];
    [command_buffer beginCommandBufferWithAllocator:allocator]; [command_buffer useResidencySet:probe_residency];
    for (unsigned pass = 0; pass < 5; ++pass) {
        if (pass == 4)
            atomic_store_explicit(&probe_buffer_lifetime.generation, 0, memory_order_release);
        const uint64_t *first_draw = pass < 2 ? draw_a : draw_a_variant;
        const uint64_t *second_draw = pass < 2 ? draw_b : draw_b_variant;
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        rp.colorAttachments[0].texture = texture; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        id<MTL4RenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:rp];
        uint64_t pso[8] = {(uint64_t)(uintptr_t)CFBridgingRetain(pipeline)};
        uint64_t dss[8] = {(uint64_t)(uintptr_t)CFBridgingRetain(depth_stencil)};
        if (!periodic_executor_begin(form_id, encoder, begin) ||
            !periodic_executor_packet(PERIODIC_METAL_PSO, pso) ||
            !periodic_executor_packet(PERIODIC_METAL_DSS, dss) ||
            !periodic_executor_packet(PERIODIC_METAL_CULL, cull) ||
            !periodic_executor_packet(PERIODIC_METAL_WINDING, winding) ||
            !periodic_executor_packet(PERIODIC_METAL_FILL, fill) ||
            !periodic_executor_packet(PERIODIC_METAL_BIAS, bias) ||
            !periodic_executor_packet(PERIODIC_METAL_CLIP, clip) ||
            !periodic_executor_packet(PERIODIC_METAL_INDEXED, first_draw) ||
            !periodic_executor_packet(PERIODIC_METAL_INDEXED, second_draw)) return 1;
        periodic_executor_packet(PERIODIC_METAL_END, end);
        [encoder endEncoding];
        if (pass == 0 || pass == 2 || pass == 4) periodic_executor_flush();
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
        stats.fallback_clusters != 3 || stats.rejected_passes) return 1;
    printf("{\"probe\":\"periodic-executor\",\"status\":\"pass\",\"indexed\":true,\"retained_state\":true,\"buffer_lifetime_invalidated\":true,\"cache_variants\":2,\"compiled_batches\":2,\"executed_batches\":2,\"executed_draws\":4,\"hot_path_allocations\":0}\n");
    return 0;
}
