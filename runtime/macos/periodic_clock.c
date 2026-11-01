#include "periodic_clock.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define NS_PER_SECOND UINT64_C(1000000000)
// Preserve the guest's existing maximum individual step, while retaining
// elapsed active time as debt instead of silently losing a long frame.
#define MAX_STEP_NS (NS_PER_SECOND / 15)

static uint64_t add_saturated(uint64_t a, uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

void periodic_clock_init(PeriodicClock *clock, unsigned anchor_hz, unsigned display_hz) {
    memset(clock, 0, sizeof *clock);
    clock->anchor_hz = anchor_hz ? anchor_hz : 60;
    clock->display_hz = display_hz ? display_hz : 60;
    clock->nominal_step_ns = NS_PER_SECOND / clock->anchor_hz;
    if (!clock->nominal_step_ns) clock->nominal_step_ns = 1;
    clock->pending_step_ns = clock->nominal_step_ns;
}

static unsigned environment_hz(const char *name, unsigned fallback) {
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    char *end;
    unsigned long result = strtoul(value, &end, 10);
    return *end || result < 1 || result > 1000 ? fallback : (unsigned)result;
}

void periodic_clock_init_environment(PeriodicClock *clock) {
    periodic_clock_init(clock, environment_hz("TOTK_PERIODIC_ANCHOR_HZ", 60),
                        environment_hz("DM_PRESENT_HZ", 60));
}

PeriodicSimulationStamp periodic_clock_complete_step(PeriodicClock *clock,
    uint64_t active_time_ns, uint64_t pinned_ns) {
    // Only the owner of the consumed guest interval calls this function. An
    // encode completion, anchor publication or display callback cannot call it.
    uint64_t consumed = pinned_ns ? pinned_ns : clock->pending_step_ns;
    clock->completed.simulation_time_ns = add_saturated(
        clock->completed.simulation_time_ns, consumed);
    clock->completed.completed_step_ns = consumed;
    clock->completed.generation++;

    if (pinned_ns != clock->pinned_step_ns) {
        clock->has_wall_sample = 0;
        clock->debt_ns = 0;
        clock->pinned_step_ns = pinned_ns;
    }
    if (!clock->has_wall_sample || pinned_ns) {
        clock->pending_step_ns = pinned_ns ? pinned_ns : clock->nominal_step_ns;
        clock->has_wall_sample = 1;
    } else {
        uint64_t elapsed = active_time_ns > clock->last_active_time_ns
            ? active_time_ns - clock->last_active_time_ns : 0;
        clock->debt_ns = add_saturated(clock->debt_ns, elapsed);
        uint64_t step = clock->debt_ns < MAX_STEP_NS ? clock->debt_ns : MAX_STEP_NS;
        clock->debt_ns -= step;
        // The guest computes the inverse timestep; a zero interval is invalid.
        clock->pending_step_ns = step ? step : 1;
    }
    if (!clock->last_active_time_ns || active_time_ns > clock->last_active_time_ns)
        clock->last_active_time_ns = active_time_ns;
    return clock->completed;
}

void periodic_clock_restore(PeriodicClock *clock, const PeriodicClock *saved) {
    *clock = *saved;
    // The checkpoint's host epoch does not survive a process restart or load.
    // Its reached simulation state, pending interval and debt do survive.
    clock->last_active_time_ns = 0;
    clock->has_wall_sample = 0;
}

static struct {
    atomic_flag writer;
    _Atomic uint64_t sequence;
    _Atomic uint64_t suspended_at_ns;
    _Atomic uint64_t suspended_total_ns;
    unsigned reasons; // Protected by the lifecycle writer flag.
} host_gate = {.writer = ATOMIC_FLAG_INIT};

static void host_suspension(unsigned reason, int suspended, uint64_t host_time_ns) {
    if (!reason || !host_time_ns) return;
    while (atomic_flag_test_and_set_explicit(&host_gate.writer, memory_order_acquire)) {}
    atomic_fetch_add_explicit(&host_gate.sequence, 1, memory_order_acq_rel);
    unsigned previous = host_gate.reasons;
    host_gate.reasons = suspended ? previous | reason : previous & ~reason;
    uint64_t at = atomic_load_explicit(&host_gate.suspended_at_ns, memory_order_relaxed);
    if (!previous && host_gate.reasons) {
        atomic_store_explicit(&host_gate.suspended_at_ns, host_time_ns, memory_order_relaxed);
    } else if (previous && !host_gate.reasons) {
        uint64_t total = atomic_load_explicit(&host_gate.suspended_total_ns, memory_order_relaxed);
        total = add_saturated(total, host_time_ns > at ? host_time_ns - at : 0);
        atomic_store_explicit(&host_gate.suspended_total_ns, total, memory_order_relaxed);
        atomic_store_explicit(&host_gate.suspended_at_ns, 0, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&host_gate.sequence, 1, memory_order_release);
    atomic_flag_clear_explicit(&host_gate.writer, memory_order_release);
}

void periodic_clock_host_suspend(unsigned reason, uint64_t host_time_ns) {
    host_suspension(reason, 1, host_time_ns);
}

void periodic_clock_host_resume(unsigned reason, uint64_t host_time_ns) {
    host_suspension(reason, 0, host_time_ns);
}

void periodic_clock_host_active(int active, uint64_t host_time_ns) {
    host_suspension(PERIODIC_CLOCK_BACKGROUND, !active, host_time_ns);
}

uint64_t periodic_clock_active_time_ns(uint64_t host_time_ns) {
    for (;;) {
        uint64_t before = atomic_load_explicit(&host_gate.sequence, memory_order_acquire);
        if (before & 1) continue;
        uint64_t at = atomic_load_explicit(&host_gate.suspended_at_ns, memory_order_relaxed);
        uint64_t total = atomic_load_explicit(&host_gate.suspended_total_ns, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (before != atomic_load_explicit(&host_gate.sequence, memory_order_relaxed)) continue;
        if (at && host_time_ns > at) host_time_ns = at;
        return host_time_ns > total ? host_time_ns - total : 0;
    }
}

PeriodicAnchorClock periodic_clock_anchor(PeriodicSimulationStamp simulation,
    uint64_t anchor_id, uint64_t presentation_time_ns, uint64_t duration_ns) {
    return (PeriodicAnchorClock){simulation, anchor_id, presentation_time_ns, duration_ns};
}

int periodic_clock_phase(const PeriodicAnchorClock *anchor,
    uint64_t presentation_time_ns, PeriodicPhaseTime *phase) {
    if (!anchor || !phase || !anchor->simulation.generation || !anchor->duration_ns ||
        presentation_time_ns <= anchor->presentation_time_ns) return 0;
    uint64_t elapsed = presentation_time_ns - anchor->presentation_time_ns;
    if (elapsed >= anchor->duration_ns ||
        elapsed > UINT64_MAX - anchor->simulation.simulation_time_ns) return 0;
    *phase = (PeriodicPhaseTime){anchor->anchor_id, anchor->simulation.generation,
        anchor->simulation.simulation_time_ns + elapsed, presentation_time_ns,
        elapsed, anchor->duration_ns};
    return 1;
}

uint64_t periodic_clock_tick_time(uint64_t epoch_ns, uint64_t tick, unsigned hz) {
    if (!hz) return epoch_ns;
    uint64_t whole = tick / hz;
    if (whole > UINT64_MAX / NS_PER_SECOND) return UINT64_MAX;
    uint64_t fraction = (tick % hz) * NS_PER_SECOND / hz;
    return add_saturated(epoch_ns, add_saturated(whole * NS_PER_SECOND, fraction));
}
