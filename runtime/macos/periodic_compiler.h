// Cold-path compiler for retained render command forms.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t first_command;
    uint32_t command_count;
    uint32_t draw_count;
    uint32_t dynamic_slot_count;
    uint32_t resource_slot_count;
    uint32_t icb_eligible;
} PeriodicICBCluster;

typedef struct {
    uint64_t form_id;
    uint32_t command_count;
    uint32_t cluster_count;
    uint32_t draw_count;
    uint32_t eligible_draw_count;
    uint32_t dynamic_slot_count;
    uint32_t resource_slot_count;
    const PeriodicICBCluster *clusters;
} PeriodicCompiledForm;

typedef struct {
    uint64_t queued_forms;
    uint64_t compiled_forms;
    uint64_t dropped_forms;
    uint64_t clusters;
    uint64_t draws;
    uint64_t eligible_draws;
} PeriodicCompilerStats;

void periodic_compiler_init(void);
void periodic_compiler_finish(void);
int periodic_compiler_active(void);
const PeriodicCompiledForm *periodic_compiler_form(uint64_t form_id);

// Cold-path synchronization for probes, shutdown and anchor-plan assembly.
void periodic_compiler_flush(void);
PeriodicCompilerStats periodic_compiler_stats(void);
int periodic_compiler_probe(void);
