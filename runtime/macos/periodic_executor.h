// Metal 4 executor for already closed retained draw capsules.
#pragma once

#include <stdint.h>

typedef struct {
    uint64_t tracked_passes;
    uint64_t rejected_passes;
    uint64_t queued_clusters;
    uint64_t compiled_clusters;
    uint64_t executed_clusters;
    uint64_t fallback_clusters;
    uint64_t executed_draws;
} PeriodicExecutorStats;

PeriodicExecutorStats periodic_executor_stats(void);
void periodic_executor_report(void);

#ifdef __OBJC__
#import <Metal/Metal.h>

enum {
    PERIODIC_METAL_PSO,
    PERIODIC_METAL_DSS,
    PERIODIC_METAL_VIEWPORT,
    PERIODIC_METAL_SCISSOR,
    PERIODIC_METAL_CULL,
    PERIODIC_METAL_WINDING,
    PERIODIC_METAL_FILL,
    PERIODIC_METAL_BIAS,
    PERIODIC_METAL_CLIP,
    PERIODIC_METAL_STENCIL,
    PERIODIC_METAL_BLEND,
    PERIODIC_METAL_VISIBILITY,
    PERIODIC_METAL_ADDRESS,
    PERIODIC_METAL_DRAW,
    PERIODIC_METAL_INSTANCED,
    PERIODIC_METAL_INDEXED,
    PERIODIC_METAL_STORE,
    PERIODIC_METAL_BEGIN,
    PERIODIC_METAL_END,
};

typedef uint64_t (*PeriodicBufferGenerationFn)(void);
// Returns a retained MTLBuffer in buffer. The executor releases it after the
// indirect command has taken its reference.
typedef int (*PeriodicResolveBufferFn)(uint64_t address, uint64_t length,
                                      void **buffer, uint64_t *offset);
typedef void (*PeriodicMakeResidentFn)(void *allocation);

void periodic_executor_init(id<MTLDevice> device,
                            PeriodicBufferGenerationFn buffer_generation,
                            PeriodicResolveBufferFn resolve_buffer,
                            PeriodicMakeResidentFn make_resident);
void periodic_executor_finish(void);
int periodic_executor_active(void);

// The render-stream worker is the only caller of begin/packet. A return value
// of one from packet means the executor encoded or queued that draw and the
// normal packet switch must not encode it again.
int periodic_executor_begin(uint64_t form_id, id<MTL4RenderCommandEncoder> encoder,
                            const uint64_t fields[8]);
int periodic_executor_packet(uint32_t op, const uint64_t fields[8]);

// Cold-path synchronization for probes and shutdown.
void periodic_executor_flush(void);
int periodic_executor_probe(void);
#endif
