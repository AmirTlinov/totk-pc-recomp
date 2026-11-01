// Retained render-pass dependency graph for the native Metal renderer.
#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    PERIODIC_RESOURCE_READ = 1u << 0,
    PERIODIC_RESOURCE_WRITE = 1u << 1,
    PERIODIC_EXECUTION_WORDS = 32,
    PERIODIC_EXECUTION_PASSES = 2048,
};

typedef struct {
    // owner identifies the backing allocation. offset and length describe the
    // affected byte range; kind and plane distinguish independent namespaces.
    uint64_t owner;
    uint64_t offset;
    uint64_t length;
    uint32_t kind;
    uint32_t plane;
} PeriodicResourceKey;

typedef struct {
    uint64_t observed_passes;
    uint64_t observed_submissions;
    uint64_t dropped_passes;
    uint64_t ring_drops;
    uint64_t submission_overflow;
    uint64_t unknown_passes;
    uint64_t same_shape_submissions;
    uint64_t changed_resources;
    uint64_t invalidated_passes;
    uint64_t promoted_guest_ranges;
    uint64_t rejected_guest_ranges;
    uint64_t graph_generation;
    uint64_t owner_returns;
    uint64_t owner_certificates;
    uint64_t published_certificates;
    uint64_t deferred_owner_drops;
    uint64_t transition_overflow;
    uint64_t transition_replacements;
    uint32_t pass_count;
    uint32_t resource_count;
    uint32_t graph_valid;
} PeriodicStoreStats;

typedef struct {
    uint64_t certificate_id;
    uint64_t form_id;
} PeriodicPassDecision;

typedef struct {
    uint64_t graph_generation;
    uint64_t submission_id;
    uint64_t execute[PERIODIC_EXECUTION_WORDS];
    uint64_t retained[PERIODIC_EXECUTION_WORDS];
    uint64_t icb[PERIODIC_EXECUTION_WORDS];
    uint64_t form_ids[PERIODIC_EXECUTION_PASSES];
    uint64_t form_anchors[PERIODIC_EXECUTION_PASSES];
    uint32_t pass_count;
    uint32_t retained_pass_count;
    uint32_t icb_pass_count;
    uint32_t icb_cluster_count;
    uint32_t icb_draw_count;
    uint32_t graph_valid;
} PeriodicExecutionPlan;

typedef uint64_t (*PeriodicGuestProtectFn)(uint64_t address, uint64_t length);
typedef int (*PeriodicGuestChangedFn)(uint64_t address, uint64_t length, uint64_t stamp);
typedef uint64_t (*PeriodicGuestEpochFn)(void);

// The replay thread is the sole producer. It writes directly into a bounded
// tape slot; no allocation, lock or compiler work occurs in these calls.
void periodic_store_init(void);
void periodic_store_finish(void);
int periodic_store_active(void);
void periodic_store_pass_begin(uint64_t pass_id);
void periodic_store_resource(PeriodicResourceKey key, uint64_t generation,
                             uint32_t role, uint8_t access);
void periodic_store_unknown(void);
// A streamed pass provides a normalized signature of its dynamic command
// state before closing. Passes without it cannot own a continuation.
void periodic_store_pass_signature(uint64_t signature, uint64_t form_observation);
PeriodicPassDecision periodic_store_pass_end(void);
// Complete the pass after the renderer has either replayed it or consumed its
// retained certificate. This records exact stable result versions before
// publishing the owner observation to the compiler. A retained pass keeps the
// versions named by the certificate.
void periodic_store_pass_complete(int retained);
void periodic_store_submit(uint64_t submission_id);
// Anchor publication only exposes certificates already closed by the owner
// compiler. It performs no comparison, closure or phase advancement.
void periodic_store_publish_certificates(uint64_t anchor_id);

// The renderer registers its guest dirty-page tracker once. Promotion and
// protection happen on the compiler thread. The replay thread only performs a
// bounded table lookup and, when the global dirty epoch changed, a range check.
void periodic_store_set_guest_tracker(PeriodicGuestProtectFn protect_range,
                                      PeriodicGuestChangedFn range_changed,
                                      PeriodicGuestEpochFn current_epoch);
uint64_t periodic_store_guest_generation(PeriodicResourceKey key,
                                         uint64_t fallback_generation);

// Cold-path and executor-facing graph queries. Returned pass IDs are ordered
// as they occurred in the latest completed submission.
void periodic_store_flush(void);
PeriodicStoreStats periodic_store_stats(void);
int periodic_store_execution_plan(PeriodicExecutionPlan *plan);
size_t periodic_store_resource_cone(PeriodicResourceKey key,
                                    uint64_t *pass_ids, size_t capacity);
int periodic_store_probe(void);
