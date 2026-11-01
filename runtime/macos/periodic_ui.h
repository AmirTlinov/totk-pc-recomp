// Ordered UI draws with owned inputs. The replay thread builds each program;
// publication is allowed only after its GPU reference comparison completes.
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __OBJC__
#import <Metal/Metal.h>

typedef struct PeriodicUIProgram PeriodicUIProgram;
typedef struct {
    __unsafe_unretained id<MTLRenderPipelineState> pipeline;
    __unsafe_unretained id<MTLDepthStencilState> depth;
    MTLViewport viewport;
    MTLScissorRect scissor;
    uint64_t vertex[31], fragment[31];
    uint32_t vertex_mask, fragment_mask;
    uint64_t index_address, index_length;
    uint32_t primitive, index_type, indexed;
    uint32_t first, count, instances, base_instance;
    int32_t base_vertex;
    uint32_t cull, winding, fill, clip;
    float bias[3], blend[4];
} PeriodicUIDraw;

int periodic_ui_enabled(void);
// The input canvas contains the completed scene before its first UI draw.
PeriodicUIProgram *periodic_ui_begin(id<MTL4CommandBuffer> command_buffer,
    uint64_t frame, uint64_t canvas_owner, id<MTLTexture> canvas);
void periodic_ui_reject(PeriodicUIProgram *program, const char *reason);
int periodic_ui_recording(const PeriodicUIProgram *program);
// Addresses returned here refer exclusively to this program's immutable copies.
uint64_t periodic_ui_bytes(PeriodicUIProgram *program, const void *bytes, size_t size);
uint64_t periodic_ui_texture(PeriodicUIProgram *program, id<MTLTexture> texture);
void periodic_ui_read_range(PeriodicUIProgram *program, uint64_t address, uint64_t size);
void periodic_ui_write_range(PeriodicUIProgram *program, uint64_t address, uint64_t size);
void periodic_ui_hold(PeriodicUIProgram *program, id object, int resident);
int periodic_ui_append(PeriodicUIProgram *program, const PeriodicUIDraw *draw);
// Encode after all original UI draws, with no encoder open. Reference is the
// original canvas with UI; it is never modified by this comparison.
int periodic_ui_compare(PeriodicUIProgram *program, id<MTL4CommandBuffer> command_buffer,
    id<MTLTexture> reference);
// Call from this submission's GPU feedback; it releases submission ownership.
void periodic_ui_completed(PeriodicUIProgram *program, int gpu_error);
// Acquiring a program does not certify the world scene used as its background.
PeriodicUIProgram *periodic_ui_acquire(uint64_t frame, uint64_t canvas_owner);
void periodic_ui_release(PeriodicUIProgram *program);
// The caller keeps its acquired program and both textures alive through GPU
// feedback, registers the textures for residency, and owns this slot exclusively
// until that feedback. Encoding does not transfer or release the reference.
int periodic_ui_encode(PeriodicUIProgram *program, id<MTL4CommandBuffer> command_buffer,
    id<MTLTexture> scene, id<MTLTexture> destination, unsigned presentation_slot);
#endif
