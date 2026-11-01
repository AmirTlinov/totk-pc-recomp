#include "periodic_camera.h"

#ifdef PERIODIC_CAMERA_TEST_HOOK
extern void periodic_camera_snapshot_after_projection(const PeriodicCameraSnapshot *snapshot);
#endif

void periodic_camera_snapshot_store(PeriodicCameraSnapshot *snapshot,
    uint64_t frame, uint64_t projection) {
    if (!snapshot || frame == UINT64_MAX) return;
    uint64_t sequence = atomic_load_explicit(&snapshot->sequence, memory_order_relaxed);
    // This record has one writer. The odd sequence becomes visible before
    // either payload field changes, including when the ring slot wraps.
    atomic_store_explicit(&snapshot->sequence, sequence + 1, memory_order_seq_cst);
    atomic_store_explicit(&snapshot->projection, projection, memory_order_seq_cst);
#ifdef PERIODIC_CAMERA_TEST_HOOK
    periodic_camera_snapshot_after_projection(snapshot);
#endif
    atomic_store_explicit(&snapshot->frame, frame + 1, memory_order_seq_cst);
    atomic_store_explicit(&snapshot->sequence, sequence + 2, memory_order_seq_cst);
}

int periodic_camera_snapshot_load(const PeriodicCameraSnapshot *snapshot,
    uint64_t frame, uint64_t *projection) {
    if (!snapshot || !projection || frame == UINT64_MAX) return 0;
    // All record accesses participate in the same atomic order. A reader that
    // observes a changed payload must also observe the writer's odd sequence
    // or its completed sequence; it cannot accept an old frame with new data.
    uint64_t sequence = atomic_load_explicit(&snapshot->sequence, memory_order_seq_cst);
    if (sequence & 1) return 0;
    uint64_t tag = atomic_load_explicit(&snapshot->frame, memory_order_seq_cst);
    uint64_t candidate = atomic_load_explicit(&snapshot->projection, memory_order_seq_cst);
    if (tag != frame + 1 ||
        sequence != atomic_load_explicit(&snapshot->sequence, memory_order_seq_cst)) return 0;
    *projection = candidate;
    return 1;
}
