// GPU command generation from native renderer parameter blocks.
#pragma once
#include <stdint.h>

enum { PERIODIC_GPU_ARRAYS, PERIODIC_GPU_INDEX16, PERIODIC_GPU_INDEX32 };

typedef struct {
    uint64_t pipeline, depth_stencil;
    uint64_t vertex[31], fragment[31];
    uint64_t index_address, index_length;
    uint32_t vertex_mask, fragment_mask;
    uint32_t primitive, kind, first, count, instances, base_instance;
    int32_t base_vertex;
    uint32_t cull, winding, fill, clip;
    float bias, slope, clamp;
} PeriodicGPUEncodeRow;

typedef struct PeriodicGPUChunk PeriodicGPUChunk;
typedef struct { PeriodicGPUChunk *chunk; uint64_t generation; } PeriodicGPUChunkTicket;

typedef struct {
    uint64_t chunks, rows, executed_ranges, executed_draws;
    uint64_t busy_fallbacks, capacity_fallbacks, invalid_rows, stale_completions;
} PeriodicGPUEncodeStats;

int periodic_gpu_encode_active(void);
PeriodicGPUEncodeStats periodic_gpu_encode_stats(void);
void periodic_gpu_encode_report(void);
// Call only after the producer and encode worker have stopped. GPU completions
// may still arrive; their leased chunks stay alive until process teardown.
void periodic_gpu_encode_finish(void);
int periodic_gpu_encode_probe(void);

// One producer appends rows; the render-stream consumer may encode ranges as
// soon as their FIFO packet is published. Seal happens after that FIFO drains,
// before command-buffer commit. Only GPU feedback completes a sealed ticket.
int periodic_gpu_encode_append(PeriodicGPUChunkTicket ticket,
    const PeriodicGPUEncodeRow *row, uint32_t *index);
// Producer-side ownership at state changes, never a lookup by raw GPU ID.
// Set resident for pipeline/resource allocations; depth state only needs retain.
int periodic_gpu_encode_hold(PeriodicGPUChunkTicket ticket, void *object, int resident);
void periodic_gpu_encode_seal(PeriodicGPUChunkTicket ticket);
void periodic_gpu_encode_complete(PeriodicGPUChunkTicket ticket);

#ifdef __OBJC__
#import <Metal/Metal.h>
// Cold startup. Defaults off; explicitly enable TOTK_PERIODIC_GPU_ENCODE=1.
// Chunks own their residency sets; no shared renderer residency lock is needed.
void periodic_gpu_encode_init(id<MTLDevice> device, uint32_t initial_capacity);
// The command buffer must have no open encoder. Encodes one compute dispatch
// with an indirect count, before all render ranges that consume this ticket.
PeriodicGPUChunkTicket periodic_gpu_encode_begin(id<MTL4CommandBuffer> command_buffer);
// Keep viewport/scissor/blend/stencil/visibility state outside these ranges.
// The caller retains and makes source allocations/pipelines resident until GPU
// completion, and restores explicit render state before ordinary fallback draws.
int periodic_gpu_encode_execute(PeriodicGPUChunkTicket ticket,
    id<MTL4RenderCommandEncoder> encoder, uint32_t first, uint32_t count);
#endif
