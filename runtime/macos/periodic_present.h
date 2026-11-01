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
// Capture the canvas once, immediately before the first UI draw. The same
// canvas remains the post-UI source; anchor capture snapshots it again and the
// phase pass keeps only pixels changed by UI at their original coordinates.
int periodic_present_capture_scene(id<MTL4CommandBuffer> command_buffer,
                                   id<MTLTexture> canvas, uint64_t frame);
uint64_t periodic_present_capture_anchor(id<MTL4CommandBuffer> command_buffer,
                                         id<MTLTexture> source, uint64_t frame,
                                         double presentation_time, NativeMousePhase input,
                                         PeriodicSimulationStamp simulation,
                                         MTLPixelFormat output_format);
// Publish snapshots only after their command buffer has been committed. Passing
// committed=0 releases snapshots encoded into a discarded command buffer.
void periodic_present_finish_submission(id<MTL4CommandBuffer> command_buffer, int committed);
int periodic_present_phase_due(double presentation_time, NativeMousePhase input);
id<MTLTexture> periodic_present_encode_phase(id<MTL4CommandBuffer> command_buffer,
                                             unsigned slot, id<MTLTexture> destination,
                                             double presentation_time,
                                             NativeMousePhase input,
                                             PeriodicPresentState *state);
int periodic_present_probe(void);
#endif
