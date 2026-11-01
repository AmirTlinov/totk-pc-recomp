// Simulation advances on a completed guest action. Anchor publication and
// presentation requests only carry or read the resulting immutable stamp.
#pragma once

#include <stdint.h>

typedef struct {
    uint64_t generation;
    uint64_t simulation_time_ns;
    uint64_t completed_step_ns;
} PeriodicSimulationStamp;

typedef struct {
    PeriodicSimulationStamp simulation;
    uint64_t anchor_id;
    uint64_t presentation_time_ns;
    uint64_t duration_ns;
} PeriodicAnchorClock;

typedef struct {
    uint64_t anchor_id;
    uint64_t simulation_generation;
    uint64_t simulation_time_ns;
    uint64_t presentation_time_ns;
    uint64_t elapsed_ns;
    uint64_t duration_ns;
} PeriodicPhaseTime;

// Owned by the guest frame thread. A checkpoint contains no host pointers.
// The pending interval is already installed in the guest's frame-rate data.
typedef struct {
    PeriodicSimulationStamp completed;
    uint64_t pending_step_ns;
    uint64_t last_active_time_ns;
    uint64_t debt_ns;
    uint64_t nominal_step_ns;
    uint64_t pinned_step_ns;
    unsigned anchor_hz;
    unsigned display_hz;
    unsigned has_wall_sample;
} PeriodicClock;

void periodic_clock_init(PeriodicClock *clock, unsigned anchor_hz, unsigned display_hz);
void periodic_clock_init_environment(PeriodicClock *clock);
// pinned_ns is an explicit diagnostic override, zero selects active wall time.
// completed reports the interval just consumed by the guest; pending_step_ns
// reports the interval to install for its next action.
PeriodicSimulationStamp periodic_clock_complete_step(PeriodicClock *clock,
    uint64_t active_time_ns, uint64_t pinned_ns);
void periodic_clock_restore(PeriodicClock *clock, const PeriodicClock *saved);

// Lifecycle transitions are cold-path writes. The guest samples active time
// without a mutex; background duration never becomes simulation catch-up debt.
// Reasons are independent: finishing warm-up cannot resume a backgrounded app.
enum { PERIODIC_CLOCK_BACKGROUND = 1u, PERIODIC_CLOCK_WARMUP = 2u };
void periodic_clock_host_suspend(unsigned reason, uint64_t host_time_ns);
void periodic_clock_host_resume(unsigned reason, uint64_t host_time_ns);
void periodic_clock_host_active(int active, uint64_t host_time_ns);
uint64_t periodic_clock_active_time_ns(uint64_t host_time_ns);

PeriodicAnchorClock periodic_clock_anchor(PeriodicSimulationStamp simulation,
    uint64_t anchor_id, uint64_t presentation_time_ns, uint64_t duration_ns);
// A phase time is a request, not proof that any owner produced this state.
int periodic_clock_phase(const PeriodicAnchorClock *anchor,
    uint64_t presentation_time_ns, PeriodicPhaseTime *phase);
// Rational timestamp calculation prevents drift from truncating 1e9 / Hz on
// every tick. It neither changes simulation nor closes an owner continuation.
uint64_t periodic_clock_tick_time(uint64_t epoch_ns, uint64_t tick, unsigned hz);
