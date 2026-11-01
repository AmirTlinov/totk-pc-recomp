#include "periodic_clock.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(value) do { assert(value); ++checks; } while (0)
#define SECOND UINT64_C(1000000000)

static void independent_domains(void) {
    PeriodicClock clock, other_display;
    periodic_clock_init(&clock, 48, 144);
    periodic_clock_init(&other_display, 48, 240);
    CHECK(clock.pending_step_ns == SECOND / 48);
    CHECK(clock.pending_step_ns != SECOND / 60);
    unsigned anchors = 0, phases = 0;
    uint64_t previous_requested_time = 0;
    for (unsigned tick = 0; tick < 48; ++tick) {
        uint64_t wall = periodic_clock_tick_time(SECOND, tick, 48);
        PeriodicSimulationStamp simulation = periodic_clock_complete_step(&clock, wall, 0);
        PeriodicSimulationStamp other = periodic_clock_complete_step(&other_display, wall, 0);
        CHECK(simulation.simulation_time_ns == other.simulation_time_ns);
        CHECK(simulation.generation == (uint64_t)tick + 1);
        PeriodicClock before = clock;
        PeriodicAnchorClock anchor = periodic_clock_anchor(simulation, tick + 1, wall, SECOND / 48);
        ++anchors;
        // An anchor's immutable publication cannot complete another guest action.
        CHECK(!memcmp(&before, &clock, sizeof clock));
        for (unsigned offset = 1; offset <= 2; ++offset) {
            uint64_t target = periodic_clock_tick_time(SECOND, tick * 3 + offset, 144);
            PeriodicPhaseTime phase;
            CHECK(periodic_clock_phase(&anchor, target, &phase));
            CHECK(phase.simulation_generation == simulation.generation);
            CHECK(phase.simulation_time_ns == simulation.simulation_time_ns + target - wall);
            CHECK(phase.presentation_time_ns > previous_requested_time);
            previous_requested_time = phase.presentation_time_ns;
            CHECK(!memcmp(&before, &clock, sizeof clock));
            ++phases;
        }
        PeriodicPhaseTime invalid;
        CHECK(!periodic_clock_phase(&anchor, wall, &invalid));
        CHECK(!periodic_clock_phase(&anchor, wall - 1, &invalid));
        CHECK(!periodic_clock_phase(&anchor, wall + anchor.duration_ns, &invalid));
    }
    CHECK(anchors == 48 && phases == 96);
    // Two rounded initial intervals can differ from one second by one ns.
    CHECK(clock.completed.simulation_time_ns >= SECOND - 1);
    CHECK(clock.completed.simulation_time_ns <= SECOND);
    CHECK(periodic_clock_tick_time(0, 144 * 3600, 144) == 3600 * SECOND);
    CHECK(periodic_clock_tick_time(UINT64_MAX - 1, 144, 144) == UINT64_MAX);
    CHECK(periodic_clock_tick_time(17, 200, 0) == 17);
    PeriodicAnchorClock invalid = {0};
    PeriodicPhaseTime phase;
    CHECK(!periodic_clock_phase(&invalid, SECOND, &phase));
    invalid = periodic_clock_anchor((PeriodicSimulationStamp){1, UINT64_MAX - 1, 1}, 1, 1, 10);
    CHECK(!periodic_clock_phase(&invalid, 3, &phase));
}

static void long_frames_keep_time(void) {
    PeriodicClock clock;
    periodic_clock_init(&clock, 48, 144);
    periodic_clock_complete_step(&clock, SECOND, 0);
    uint64_t total_wall = 0, assigned = 0;
    const uint64_t elapsed[] = {20*1000000, 400*1000000, 10*1000000, 31*1000000,
        11*1000000, 20*1000000, 10*1000000, 19*1000000, 20*1000000, 20*1000000,
        20*1000000, 20*1000000, 20*1000000, 20*1000000};
    for (unsigned i = 0; i < sizeof elapsed / sizeof elapsed[0]; ++i) {
        total_wall += elapsed[i];
        uint64_t consumed = clock.pending_step_ns;
        uint64_t previous_simulation = clock.completed.simulation_time_ns;
        PeriodicSimulationStamp stamp = periodic_clock_complete_step(&clock, SECOND + total_wall, 0);
        CHECK(stamp.simulation_time_ns == previous_simulation + consumed);
        CHECK(stamp.completed_step_ns == consumed);
        CHECK(clock.pending_step_ns <= SECOND / 15);
        assigned += clock.pending_step_ns;
        CHECK(assigned + clock.debt_ns == total_wall);
    }
    CHECK(!clock.debt_ns);
    CHECK(clock.pending_step_ns == elapsed[sizeof elapsed / sizeof elapsed[0] - 1]);
}

static void suspension_and_restore(void) {
    PeriodicClock clock;
    periodic_clock_init(&clock, 48, 144);
    periodic_clock_complete_step(&clock, periodic_clock_active_time_ns(10 * SECOND), 0);
    periodic_clock_host_active(0, 10 * SECOND + 10 * 1000000);
    // Repeated inactive notification cannot restart the suspended interval.
    periodic_clock_host_active(0, 11 * SECOND);
    CHECK(periodic_clock_active_time_ns(19 * SECOND) == 10 * SECOND + 10 * 1000000);
    periodic_clock_host_active(1, 20 * SECOND);
    periodic_clock_host_active(1, 20 * SECOND + 1);
    periodic_clock_complete_step(&clock,
        periodic_clock_active_time_ns(20 * SECOND + 10 * 1000000), 0);
    CHECK(clock.pending_step_ns == 20 * 1000000);
    CHECK(!clock.debt_ns);

    PeriodicClock saved = clock, restored;
    periodic_clock_restore(&restored, &saved);
    CHECK(restored.completed.simulation_time_ns == saved.completed.simulation_time_ns);
    CHECK(restored.completed.generation == saved.completed.generation);
    CHECK(restored.pending_step_ns == saved.pending_step_ns);
    CHECK(restored.debt_ns == saved.debt_ns);
    CHECK(!restored.has_wall_sample);
    PeriodicSimulationStamp resumed = periodic_clock_complete_step(&restored, 100 * SECOND, 0);
    CHECK(resumed.simulation_time_ns == saved.completed.simulation_time_ns + saved.pending_step_ns);
    CHECK(!restored.debt_ns);
    CHECK(restored.pending_step_ns == SECOND / 48);
}

static void warmup_does_not_create_catchup(void) {
    PeriodicClock clock;
    periodic_clock_init(&clock, 48, 144);
    periodic_clock_complete_step(&clock, periodic_clock_active_time_ns(100 * SECOND), 0);
    periodic_clock_host_suspend(PERIODIC_CLOCK_WARMUP, 100 * SECOND + 10 * 1000000);
    uint64_t frozen = periodic_clock_active_time_ns(100 * SECOND + 10 * 1000000);
    periodic_clock_host_active(0, 101 * SECOND);
    CHECK(periodic_clock_active_time_ns(104 * SECOND) == frozen);
    periodic_clock_host_resume(PERIODIC_CLOCK_WARMUP, 105 * SECOND);
    CHECK(periodic_clock_active_time_ns(106 * SECOND) == frozen);
    periodic_clock_host_active(1, 107 * SECOND);
    for (unsigned frame = 0; frame < 8; ++frame) {
        periodic_clock_complete_step(&clock, periodic_clock_active_time_ns(
            107 * SECOND + (10 + frame * 20) * 1000000), 0);
        CHECK(clock.pending_step_ns == 20 * 1000000);
        CHECK(!clock.debt_ns);
    }
    // Opposite nesting order: foreground restoration does not finish warm-up.
    periodic_clock_host_active(0, 110 * SECOND);
    frozen = periodic_clock_active_time_ns(110 * SECOND);
    periodic_clock_host_suspend(PERIODIC_CLOCK_WARMUP, 111 * SECOND);
    periodic_clock_host_active(1, 112 * SECOND);
    CHECK(periodic_clock_active_time_ns(113 * SECOND) == frozen);
    periodic_clock_host_resume(PERIODIC_CLOCK_WARMUP, 114 * SECOND);
    CHECK(periodic_clock_active_time_ns(114 * SECOND + 20 * 1000000) == frozen + 20 * 1000000);
}

int main(void) {
    independent_domains();
    long_frames_keep_time();
    suspension_and_restore();
    warmup_does_not_create_catchup();
    printf("periodic clocks: %u checks passed; 48 anchor stamps + 96 phase-time requests, no simulation advance from presentation\n", checks);
    return 0;
}
