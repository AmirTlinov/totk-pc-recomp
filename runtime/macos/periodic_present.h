// Retained display-phase composition for the native Metal renderer.
#pragma once

#include <stdint.h>
#include "mouse_input.h"

typedef struct {
    uint64_t source_frame;
    uint64_t anchor_id;
    uint64_t state_generation;
    double phase;
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
uint64_t periodic_present_capture_anchor(id<MTL4CommandBuffer> command_buffer,
                                         id<MTLTexture> source, uint64_t frame,
                                         double presentation_time, NativeMousePhase input,
                                         MTLPixelFormat output_format);
int periodic_present_phase_due(double presentation_time, NativeMousePhase input);
id<MTLTexture> periodic_present_encode_phase(id<MTL4CommandBuffer> command_buffer,
                                             unsigned slot, id<MTLTexture> destination,
                                             double presentation_time,
                                             NativeMousePhase input,
                                             PeriodicPresentState *state);
int periodic_present_probe(void);
#endif
