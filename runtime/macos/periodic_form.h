// Retained command forms for the native Metal renderer.
#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    PERIODIC_PACKET_PASS_BEGIN = 1u << 0,
    PERIODIC_PACKET_PASS_END = 1u << 1,
    PERIODIC_PACKET_DRAW = 1u << 2,
};

typedef struct {
    uint32_t role;
    uint32_t generation;
    uint64_t serial;
    uint64_t offset;
} PeriodicResourceRef;

typedef struct {
    uint64_t observed_packets;
    uint64_t observed_anchors;
    uint64_t dropped_records;
    uint64_t learned_forms;
    uint64_t closed_forms;
    uint64_t invalidated_forms;
    uint64_t state_generation;
} PeriodicFormStats;

// Initialization, compilation and persistence stay outside the render path.
// Packet observation is a bounded SPSC append and never waits or allocates.
void periodic_form_init(void);
void periodic_form_finish(void);
int periodic_form_active(void);
void periodic_observe_packet(uint32_t op, const uint64_t fields[8], uint8_t static_mask,
                             uint8_t dynamic_mask, uint8_t resource_mask, uint8_t flags,
                             const PeriodicResourceRef resources[8]);

// An anchor is a guest-rendered state. A phase renderer may publish additional
// states without advancing the guest simulation clock.
uint64_t periodic_anchor_publish(uint64_t anchor_id);
uint64_t periodic_anchor_generation(uint64_t anchor_id);
PeriodicFormStats periodic_form_stats(void);

// Waits for the compiler only in probes, shutdown and other cold paths.
void periodic_form_flush(void);
int periodic_form_probe(void);
