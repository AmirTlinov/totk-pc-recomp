#include "periodic_gpu_encode.h"
#import <Foundation/Foundation.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(PeriodicGPUEncodeRow) == 592, "Metal parameter row ABI");
_Static_assert(offsetof(PeriodicGPUEncodeRow, index_address) == 512, "Metal index binding ABI");
_Static_assert(offsetof(PeriodicGPUEncodeRow, bias) == 580, "Metal raster state ABI");

static NSString *const encode_source =
    @"#include <metal_stdlib>\n#include <metal_command_buffer>\nusing namespace metal;\n"
     "struct Control {command_buffer commands [[id(0)]];};\n"
     "struct Row {render_pipeline_state pipeline;depth_stencil_state depth_stencil;"
     "device uchar *vertex_buffers[31];device uchar *fragment_buffers[31];device uchar *indices;ulong index_length;"
     "uint vertex_mask,fragment_mask,primitive,kind,first,count,instances,base_instance;int base_vertex;"
     "uint cull,winding,fill,clip;float bias,slope,clamp;};\n"
     "static_assert(sizeof(Row)==592,\"native row ABI\");\n"
     "kernel void build_commands(constant Control &control [[buffer(0)]],device const Row *rows [[buffer(1)]],uint i [[thread_position_in_grid]]) {"
     "device const Row &r=rows[i];render_command command(control.commands,i);command.reset();"
     "command.set_render_pipeline_state(r.pipeline);command.set_depth_stencil_state(r.depth_stencil);"
     "command.set_cull_mode(cull_mode(r.cull));command.set_front_facing_winding(winding(r.winding));"
     "command.set_triangle_fill_mode(triangle_fill_mode(r.fill));command.set_depth_clip_mode(depth_clip_mode(r.clip));"
     "command.set_depth_bias(r.bias,r.slope,r.clamp);"
     "uint bindings=r.vertex_mask;while(bindings){uint slot=ctz(bindings);command.set_vertex_buffer(r.vertex_buffers[slot],slot);bindings&=bindings-1;}"
     "bindings=r.fragment_mask;while(bindings){uint slot=ctz(bindings);command.set_fragment_buffer(r.fragment_buffers[slot],slot);bindings&=bindings-1;}"
     "if(r.kind==0)command.draw_primitives(primitive_type(r.primitive),r.first,r.count,r.instances,r.base_instance);"
     "else if(r.kind==1)command.draw_indexed_primitives(primitive_type(r.primitive),r.count,reinterpret_cast<device ushort*>(r.indices),r.instances,uint(r.base_vertex),r.base_instance);"
     "else command.draw_indexed_primitives(primitive_type(r.primitive),r.count,reinterpret_cast<device uint*>(r.indices),r.instances,uint(r.base_vertex),r.base_instance);}\n";

struct PeriodicGPUChunk {
    struct PeriodicGPUChunk *next;
    _Atomic uint64_t lease;
    _Atomic uint32_t count;
    _Atomic int sealed;
    uint32_t capacity, hold_capacity, held_count;
    int accepting;
    __strong id<MTLIndirectCommandBuffer> commands;
    __strong id<MTLBuffer> rows, dispatch, control;
    __strong id<MTL4ArgumentTable> table;
    __strong id<MTLResidencySet> residency;
    __strong id *held;
    uint8_t *held_resident;
};
static struct {
    __strong id<MTLDevice> device;
    __strong id<MTLFunction> function;
    __strong id<MTLComputePipelineState> pipeline;
    __strong id<MTLDepthStencilState> default_depth;
    _Atomic(PeriodicGPUChunk *) chunks;
    _Atomic uint32_t preferred_capacity;
    _Atomic uint64_t next_lease;
    _Atomic int active, stopping, growth;
    dispatch_semaphore_t grow_ready;
    pthread_t grower;
    _Atomic uint64_t chunk_count, rows, ranges, draws;
    _Atomic uint64_t busy, capacity, invalid, stale;
} gpu_encode;

static int ticket_valid(PeriodicGPUChunkTicket ticket) {
    return ticket.chunk && ticket.generation &&
           atomic_load_explicit(&ticket.chunk->lease, memory_order_acquire) == ticket.generation;
}
static void destroy_chunk(PeriodicGPUChunk *chunk) {
    for (uint32_t i = 0; i < chunk->held_count; ++i)
        chunk->held[i] = nil;
    chunk->residency = nil;
    chunk->table = nil;
    chunk->control = nil;
    chunk->dispatch = nil;
    chunk->rows = nil;
    chunk->commands = nil;
    free(chunk->held);
    free(chunk->held_resident);
    free(chunk);
}
static int make_chunk(uint32_t capacity) {
    if (!capacity || capacity > (UINT32_MAX - 8) / 2)
        return 0;
    PeriodicGPUChunk *chunk = calloc(1, sizeof *chunk);
    if (!chunk)
        return 0;
    chunk->capacity = capacity;
    chunk->hold_capacity = capacity * 2 + 8;
    chunk->held = (__strong id *)calloc(chunk->hold_capacity, sizeof(id));
    chunk->held_resident = calloc(chunk->hold_capacity, 1);
    NSError *error = nil;
    MTLIndirectCommandBufferDescriptor *descriptor = [MTLIndirectCommandBufferDescriptor new];
    descriptor.commandTypes = MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed;
    descriptor.inheritPipelineState = NO;
    descriptor.inheritBuffers = NO;
    descriptor.inheritDepthStencilState = NO;
    descriptor.inheritCullMode = NO;
    descriptor.inheritFrontFacingWinding = NO;
    descriptor.inheritTriangleFillMode = NO;
    descriptor.inheritDepthBias = NO;
    descriptor.inheritDepthClipMode = NO;
    descriptor.maxVertexBufferBindCount = descriptor.maxFragmentBufferBindCount = 31;
    chunk->commands =
        [gpu_encode.device newIndirectCommandBufferWithDescriptor:descriptor
                                                  maxCommandCount:capacity
                                                          options:MTLResourceStorageModePrivate];
    chunk->rows =
        [gpu_encode.device newBufferWithLength:(NSUInteger)capacity * sizeof(PeriodicGPUEncodeRow)
                                       options:MTLResourceStorageModeShared];
    chunk->dispatch =
        [gpu_encode.device newBufferWithLength:sizeof(MTLDispatchThreadsIndirectArguments)
                                       options:MTLResourceStorageModeShared];
    id<MTLArgumentEncoder> encoder = [gpu_encode.function newArgumentEncoderWithBufferIndex:0];
    chunk->control = [gpu_encode.device newBufferWithLength:encoder.encodedLength
                                                    options:MTLResourceStorageModeShared];
    MTL4ArgumentTableDescriptor *table = [MTL4ArgumentTableDescriptor new];
    table.maxBufferBindCount = 2;
    table.initializeBindings = YES;
    chunk->table = [gpu_encode.device newArgumentTableWithDescriptor:table error:&error];
    MTLResidencySetDescriptor *residency = [MTLResidencySetDescriptor new];
    residency.initialCapacity = chunk->hold_capacity + 4;
    chunk->residency = [gpu_encode.device newResidencySetWithDescriptor:residency error:&error];
    if (!chunk->held || !chunk->held_resident || !chunk->commands || !chunk->rows ||
        !chunk->dispatch || !chunk->control || !chunk->table || !chunk->residency) {
        destroy_chunk(chunk);
        return 0;
    }
    [encoder setArgumentBuffer:chunk->control offset:0];
    [encoder setIndirectCommandBuffer:chunk->commands atIndex:0];
    [chunk->table setAddress:chunk->control.gpuAddress atIndex:0];
    [chunk->table setAddress:chunk->rows.gpuAddress atIndex:1];
    id<MTLAllocation> objects[] = {chunk->commands, chunk->rows, chunk->dispatch, chunk->control};
    [chunk->residency addAllocations:objects count:4];
    [chunk->residency commit];
    PeriodicGPUChunk *head = atomic_load_explicit(&gpu_encode.chunks, memory_order_relaxed);
    do {
        chunk->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&gpu_encode.chunks, &head, chunk,
                                                    memory_order_release, memory_order_relaxed));
    return 1;
}
static void request_growth(uint32_t capacity) {
    uint32_t current = atomic_load_explicit(&gpu_encode.preferred_capacity, memory_order_relaxed);
    while (current < capacity && !atomic_compare_exchange_weak_explicit(
                                     &gpu_encode.preferred_capacity, &current, capacity,
                                     memory_order_release, memory_order_relaxed)) {
    }
    if (!atomic_exchange_explicit(&gpu_encode.growth, 1, memory_order_acq_rel))
        dispatch_semaphore_signal(gpu_encode.grow_ready);
}
static void *grow_chunks(void *unused) {
    (void)unused;
    pthread_setname_np("TOTK GPU parameter storage");
    for (;;)
        @autoreleasepool {
            dispatch_semaphore_wait(gpu_encode.grow_ready, DISPATCH_TIME_FOREVER);
            if (atomic_load_explicit(&gpu_encode.stopping, memory_order_acquire))
                break;
            uint32_t capacity =
                atomic_load_explicit(&gpu_encode.preferred_capacity, memory_order_acquire);
            for (unsigned i = 0; i < 2; ++i)
                if (!make_chunk(capacity))
                    break;
            atomic_store_explicit(&gpu_encode.growth, 0, memory_order_release);
        }
    return NULL;
}
void periodic_gpu_encode_init(id<MTLDevice> device, uint32_t capacity) {
    const char *enabled = getenv("TOTK_PERIODIC_GPU_ENCODE");
    if (periodic_gpu_encode_active() || !device || !enabled || strcmp(enabled, "1"))
        return;
    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion4_0;
    options.mathMode = MTLMathModeSafe;
    options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
    id<MTLLibrary> library = [device newLibraryWithSource:encode_source
                                                  options:options
                                                    error:&error];
    gpu_encode.device = device;
    gpu_encode.function = [library newFunctionWithName:@"build_commands"];
    gpu_encode.pipeline = gpu_encode.function
                              ? [device newComputePipelineStateWithFunction:gpu_encode.function
                                                                      error:&error]
                              : nil;
    MTLDepthStencilDescriptor *depth = [MTLDepthStencilDescriptor new];
    depth.depthCompareFunction = MTLCompareFunctionAlways;
    gpu_encode.default_depth = [device newDepthStencilStateWithDescriptor:depth];
    if (!gpu_encode.pipeline || !gpu_encode.default_depth) {
        fprintf(stderr, "[totk] GPU command program unavailable: %s\n",
                error.description.UTF8String);
        return;
    }
    if (!capacity)
        capacity = 4096;
    atomic_store(&gpu_encode.preferred_capacity, capacity);
    gpu_encode.grow_ready = dispatch_semaphore_create(0);
    for (unsigned i = 0; i < 4; ++i)
        if (!make_chunk(capacity))
            break;
    if (!atomic_load(&gpu_encode.chunks) ||
        pthread_create(&gpu_encode.grower, NULL, grow_chunks, NULL))
        return;
    // Process shutdown can leave the renderer threads running. Report here;
    // explicit teardown requires their owner to quiesce them first.
    atomic_store_explicit(&gpu_encode.active, 1, memory_order_release);
    atexit(periodic_gpu_encode_report);
}
int periodic_gpu_encode_active(void) {
    return atomic_load_explicit(&gpu_encode.active, memory_order_acquire);
}
PeriodicGPUChunkTicket periodic_gpu_encode_begin(id<MTL4CommandBuffer> cb) {
    PeriodicGPUChunkTicket none = {0};
    if (!periodic_gpu_encode_active() || !cb)
        return none;
    uint32_t wanted = atomic_load_explicit(&gpu_encode.preferred_capacity, memory_order_acquire);
    PeriodicGPUChunk *chunk = atomic_load_explicit(&gpu_encode.chunks, memory_order_acquire);
    uint64_t generation =
        atomic_fetch_add_explicit(&gpu_encode.next_lease, 1, memory_order_relaxed) + 1;
    if (!generation)
        return none;
    for (; chunk; chunk = chunk->next) {
        uint64_t empty = 0;
        if (chunk->capacity >= wanted &&
            atomic_compare_exchange_strong_explicit(&chunk->lease, &empty, generation,
                                                    memory_order_acquire, memory_order_relaxed))
            break;
    }
    if (!chunk) {
        atomic_fetch_add_explicit(&gpu_encode.busy, 1, memory_order_relaxed);
        request_growth(wanted);
        return none;
    }
    // A slot's own completion made these old references safe to retire. Other
    // in-flight chunks retain their independent objects and residency sets.
    for (uint32_t i = 0; i < chunk->held_count; ++i) {
        if (chunk->held_resident[i])
            [chunk->residency removeAllocation:(id<MTLAllocation>)chunk->held[i]];
        chunk->held[i] = nil;
    }
    chunk->held_count = 0;
    chunk->accepting = 1;
    atomic_store(&chunk->count, 0);
    atomic_store(&chunk->sealed, 0);
    memset(chunk->dispatch.contents, 0, sizeof(MTLDispatchThreadsIndirectArguments));
    [cb useResidencySet:chunk->residency];
    id<MTL4ComputeCommandEncoder> encoder = [cb computeCommandEncoder];
    if (!encoder) {
        atomic_store_explicit(&chunk->lease, 0, memory_order_release);
        return none;
    }
    [encoder setArgumentTable:chunk->table];
    [encoder setComputePipelineState:gpu_encode.pipeline];
    [encoder dispatchThreadsWithIndirectBuffer:chunk->dispatch.gpuAddress];
    [encoder barrierAfterStages:MTLStageDispatch
              beforeQueueStages:MTLStageAll
              visibilityOptions:MTL4VisibilityOptionDevice | MTL4VisibilityOptionResourceAlias];
    [encoder endEncoding];
    atomic_fetch_add_explicit(&gpu_encode.chunk_count, 1, memory_order_relaxed);
    return (PeriodicGPUChunkTicket){chunk, generation};
}
int periodic_gpu_encode_hold(PeriodicGPUChunkTicket ticket, void *object, int resident) {
    if (!object)
        return ticket_valid(ticket);
    if (!ticket_valid(ticket) ||
        atomic_load_explicit(&ticket.chunk->sealed, memory_order_acquire) ||
        !ticket.chunk->accepting)
        return 0;
    PeriodicGPUChunk *chunk = ticket.chunk;
    id held = (__bridge id)object;
    if (chunk->held_count && chunk->held[chunk->held_count - 1] == held &&
        chunk->held_resident[chunk->held_count - 1] == !!resident)
        return 1;
    if (chunk->held_count == chunk->hold_capacity) {
        chunk->accepting = 0;
        atomic_fetch_add_explicit(&gpu_encode.capacity, 1, memory_order_relaxed);
        if (chunk->capacity < UINT32_MAX / 2)
            request_growth(chunk->capacity * 2);
        return 0;
    }
    chunk->held[chunk->held_count] = held;
    chunk->held_resident[chunk->held_count++] = !!resident;
    if (resident)
        [chunk->residency addAllocation:(id<MTLAllocation>)held];
    return 1;
}
int periodic_gpu_encode_append(PeriodicGPUChunkTicket ticket, const PeriodicGPUEncodeRow *row,
                               uint32_t *index) {
    if (!row || !ticket_valid(ticket) ||
        atomic_load_explicit(&ticket.chunk->sealed, memory_order_acquire) ||
        !ticket.chunk->accepting)
        return 0;
    uint64_t index_size = row->kind == PERIODIC_GPU_INDEX16   ? 2
                          : row->kind == PERIODIC_GPU_INDEX32 ? 4
                                                              : 0;
    if (!row->pipeline || row->kind > PERIODIC_GPU_INDEX32 ||
        row->primitive > MTLPrimitiveTypeTriangleStrip || (row->vertex_mask >> 31) ||
        (row->fragment_mask >> 31) || row->cull > MTLCullModeBack ||
        row->winding > MTLWindingCounterClockwise || row->fill > MTLTriangleFillModeLines ||
        row->clip > MTLDepthClipModeClamp ||
        (index_size && (!row->index_address || row->count > row->index_length / index_size ||
                        row->index_address % index_size || row->index_length % index_size))) {
        atomic_fetch_add_explicit(&gpu_encode.invalid, 1, memory_order_relaxed);
        return 0;
    }
    PeriodicGPUChunk *chunk = ticket.chunk;
    uint32_t count = atomic_load_explicit(&chunk->count, memory_order_relaxed);
    if (count == chunk->capacity) {
        chunk->accepting = 0;
        atomic_fetch_add_explicit(&gpu_encode.capacity, 1, memory_order_relaxed);
        if (count < UINT32_MAX / 2)
            request_growth(count * 2);
        return 0;
    }
    PeriodicGPUEncodeRow *destination = (PeriodicGPUEncodeRow *)chunk->rows.contents + count;
    *destination = *row;
    if (!destination->depth_stencil)
        destination->depth_stencil = gpu_encode.default_depth.gpuResourceID._impl;
    atomic_store_explicit(&chunk->count, count + 1, memory_order_release);
    if (index)
        *index = count;
    atomic_fetch_add_explicit(&gpu_encode.rows, 1, memory_order_relaxed);
    return 1;
}
int periodic_gpu_encode_execute(PeriodicGPUChunkTicket ticket, id<MTL4RenderCommandEncoder> encoder,
                                uint32_t first, uint32_t count) {
    if (!encoder || !count || !ticket_valid(ticket))
        return 0;
    uint32_t available = atomic_load_explicit(&ticket.chunk->count, memory_order_acquire);
    if (first > available || count > available - first)
        return 0;
    [encoder executeCommandsInBuffer:ticket.chunk->commands withRange:NSMakeRange(first, count)];
    atomic_fetch_add_explicit(&gpu_encode.ranges, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&gpu_encode.draws, count, memory_order_relaxed);
    return 1;
}
void periodic_gpu_encode_seal(PeriodicGPUChunkTicket ticket) {
    if (!ticket_valid(ticket) || atomic_load_explicit(&ticket.chunk->sealed, memory_order_acquire))
        return;
    PeriodicGPUChunk *chunk = ticket.chunk;
    uint32_t count = atomic_load_explicit(&chunk->count, memory_order_acquire);
    MTLDispatchThreadsIndirectArguments dispatch = {{count, 1, 1}, {64, 1, 1}};
    memcpy(chunk->dispatch.contents, &dispatch, sizeof dispatch);
    [chunk->residency commit];
    atomic_store_explicit(&chunk->sealed, 1, memory_order_release);
}
void periodic_gpu_encode_complete(PeriodicGPUChunkTicket ticket) {
    if (!ticket.chunk || !ticket.generation)
        return;
    uint64_t expected = ticket.generation;
    if (!atomic_load_explicit(&ticket.chunk->sealed, memory_order_acquire) ||
        !atomic_compare_exchange_strong_explicit(&ticket.chunk->lease, &expected, 0,
                                                 memory_order_release, memory_order_relaxed))
        atomic_fetch_add_explicit(&gpu_encode.stale, 1, memory_order_relaxed);
}
PeriodicGPUEncodeStats periodic_gpu_encode_stats(void) {
    return (PeriodicGPUEncodeStats){.chunks = atomic_load(&gpu_encode.chunk_count),
                                    .rows = atomic_load(&gpu_encode.rows),
                                    .executed_ranges = atomic_load(&gpu_encode.ranges),
                                    .executed_draws = atomic_load(&gpu_encode.draws),
                                    .busy_fallbacks = atomic_load(&gpu_encode.busy),
                                    .capacity_fallbacks = atomic_load(&gpu_encode.capacity),
                                    .invalid_rows = atomic_load(&gpu_encode.invalid),
                                    .stale_completions = atomic_load(&gpu_encode.stale)};
}
void periodic_gpu_encode_report(void) {
    PeriodicGPUEncodeStats s = periodic_gpu_encode_stats();
    if (s.chunks)
        fprintf(
            stderr,
            "[totk] GPU command generation: %llu chunks, %llu rows, %llu ranges / %llu draws; busy=%llu capacity=%llu invalid=%llu stale=%llu\n",
            (unsigned long long)s.chunks, (unsigned long long)s.rows,
            (unsigned long long)s.executed_ranges, (unsigned long long)s.executed_draws,
            (unsigned long long)s.busy_fallbacks, (unsigned long long)s.capacity_fallbacks,
            (unsigned long long)s.invalid_rows, (unsigned long long)s.stale_completions);
}
void periodic_gpu_encode_finish(void) {
    if (!atomic_exchange_explicit(&gpu_encode.active, 0, memory_order_acq_rel))
        return;
    atomic_store_explicit(&gpu_encode.stopping, 1, memory_order_release);
    dispatch_semaphore_signal(gpu_encode.grow_ready);
    pthread_join(gpu_encode.grower, NULL);
    periodic_gpu_encode_report();
    int inflight = 0;
    PeriodicGPUChunk *chunk = atomic_exchange(&gpu_encode.chunks, NULL);
    while (chunk) {
        PeriodicGPUChunk *next = chunk->next;
        if (!atomic_load_explicit(&chunk->lease, memory_order_acquire))
            destroy_chunk(chunk);
        else
            inflight = 1;
        chunk = next;
    }
    if (!inflight) {
        gpu_encode.default_depth = nil;
        gpu_encode.pipeline = nil;
        gpu_encode.function = nil;
        gpu_encode.device = nil;
    }
    gpu_encode.grow_ready = NULL;
}
