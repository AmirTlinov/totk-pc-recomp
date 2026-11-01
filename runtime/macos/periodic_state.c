#include "periodic_state.h"
#include <stdatomic.h>

enum { ANCHOR_SLOTS = 256 };
static _Atomic uint64_t publication, latest;
static struct { _Atomic uint64_t sequence, anchor, generation; } anchors[ANCHOR_SLOTS];

uint64_t periodic_anchor_publish(uint64_t anchor_id) {
    uint64_t generation = atomic_fetch_add_explicit(&publication, 1, memory_order_relaxed) + 1;
    unsigned slot = (unsigned)(anchor_id % ANCHOR_SLOTS);
    // One anchor publisher; readers must not pair an old identity with the
    // generation of a newer anchor when the diagnostic ring wraps.
    atomic_fetch_add_explicit(&anchors[slot].sequence, 1, memory_order_acq_rel);
    atomic_store_explicit(&anchors[slot].generation, generation, memory_order_relaxed);
    atomic_store_explicit(&anchors[slot].anchor, anchor_id + 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&anchors[slot].sequence, 1, memory_order_release);
    atomic_store_explicit(&latest, anchor_id + 1, memory_order_release);
    return generation;
}
uint64_t periodic_phase_publish(void) {
    return atomic_fetch_add_explicit(&publication, 1, memory_order_relaxed) + 1;
}
uint64_t periodic_latest_anchor(void) {
    uint64_t anchor = atomic_load_explicit(&latest, memory_order_acquire);
    return anchor ? anchor - 1 : UINT64_MAX;
}
uint64_t periodic_anchor_generation(uint64_t anchor_id) {
    unsigned slot = (unsigned)(anchor_id % ANCHOR_SLOTS);
    uint64_t sequence = atomic_load_explicit(&anchors[slot].sequence, memory_order_acquire);
    if (sequence & 1) return 0;
    uint64_t identity = atomic_load_explicit(&anchors[slot].anchor, memory_order_relaxed);
    uint64_t generation = atomic_load_explicit(&anchors[slot].generation, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if (identity != anchor_id + 1 ||
        atomic_load_explicit(&anchors[slot].sequence, memory_order_acquire) != sequence) return 0;
    return generation;
}
