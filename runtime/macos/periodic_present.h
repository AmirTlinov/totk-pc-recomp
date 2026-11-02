// Retained display-phase composition for the native Metal renderer.
#pragma once

#include <stdint.h>
#include "mouse_input.h"
#include "periodic_clock.h"

typedef struct {
    uint64_t source_frame;
    uint64_t anchor_id;
    uint64_t state_generation;
    double phase;
    PeriodicPhaseTime time;
} PeriodicPresentState;

typedef struct {
    float vertical_fov;
    float aspect;
} PeriodicCamera;

void periodic_present_note_camera(uint64_t frame, const float *camera, unsigned count);
PeriodicCamera periodic_present_camera(uint64_t anchor_frame);

#ifdef __OBJC__
#import <Metal/Metal.h>

typedef void (*PeriodicPresentResidencyFn)(id<MTLAllocation> allocation);

// Resource creation and shader compilation are cold-path work. Anchor capture
// may replace resources after a size/format change. Phase encode only updates a
// mapped constant block and records GPU commands.
void periodic_present_prepare(id<MTLDevice> device, PeriodicPresentResidencyFn residency);
int periodic_present_enabled(void);
// Convert a guest frame's normal display-link stride into the configured
// anchor stride. Display ticks left between anchors belong to retained phases.
unsigned periodic_present_anchor_ticks(PeriodicAnchorCadence *, unsigned display_hz, unsigned fallback_ticks);
// Share the UI owner's immutable pre-UI image. Publish only after the command
// buffer containing its copy has entered the queue. Canvas identifies the exact
// UI program; a previous frame cannot satisfy either input.
int periodic_present_share_scene(id<MTL4CommandBuffer> command_buffer,
    id<MTLTexture> canvas, id<MTLTexture> scene, uint64_t frame);
uint64_t periodic_present_capture_anchor(id<MTL4CommandBuffer> command_buffer,
                                         id<MTLTexture> source, uint64_t frame,
                                         double presentation_time, NativeMousePhase input,
                                         PeriodicSimulationStamp simulation);
// Publish snapshots only after their command buffer has been committed. Passing
// committed=0 releases snapshots encoded into a discarded command buffer.
void periodic_present_finish_submission(id<MTL4CommandBuffer> command_buffer, int committed);
int periodic_present_phase_due(double presentation_time, NativeMousePhase input);
id<MTLTexture> periodic_present_encode_phase(id<MTL4CommandBuffer> command_buffer,
                                             unsigned slot, id<MTLTexture> destination,
                                             double presentation_time,
                                             NativeMousePhase input,
                                             PeriodicPresentState *state);
// Call exactly once from this slot's GPU feedback, before releasing its command
// allocator. A phase holds both snapshots until the GPU has finished reading.
void periodic_present_complete_phase(unsigned slot);
int periodic_present_probe(void);
#endif
