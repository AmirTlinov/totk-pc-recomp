// Retained render-pass dependency graph for the native Metal renderer.
#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    PERIODIC_RESOURCE_READ = 1u << 0,
    PERIODIC_RESOURCE_WRITE = 1u << 1,
    PERIODIC_EXECUTION_WORDS = 8,
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
    uint64_t unknown_passes;
    uint64_t same_shape_submissions;
    uint64_t changed_resources;
    uint64_t invalidated_passes;
    uint64_t graph_generation;
    uint32_t pass_count;
    uint32_t resource_count;
    uint32_t graph_valid;
} PeriodicStoreStats;

typedef struct {
    uint64_t graph_generation;
    uint64_t submission_id;
    uint64_t execute[PERIODIC_EXECUTION_WORDS];
    uint32_t pass_count;
    uint32_t graph_valid;
} PeriodicExecutionPlan;

// The replay thread is the sole producer. It writes directly into a bounded
// tape slot; no allocation, lock or compiler work occurs in these calls.
void periodic_store_init(void);
void periodic_store_finish(void);
int periodic_store_active(void);
void periodic_store_pass_begin(uint64_t pass_id);
void periodic_store_resource(PeriodicResourceKey key, uint64_t generation,
                             uint32_t role, uint8_t access);
void periodic_store_unknown(void);
void periodic_store_pass_end(void);
void periodic_store_submit(uint64_t submission_id);

// Cold-path and executor-facing graph queries. Returned pass IDs are ordered
// as they occurred in the latest completed submission.
void periodic_store_flush(void);
PeriodicStoreStats periodic_store_stats(void);
int periodic_store_execution_plan(PeriodicExecutionPlan *plan);
size_t periodic_store_resource_cone(PeriodicResourceKey key,
                                    uint64_t *pass_ids, size_t capacity);
int periodic_store_probe(void);
