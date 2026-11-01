#include "mouse_input.h"
#include "input_bindings.h"
#include <math.h>
#include <stdatomic.h>

// Nanoradians keep the cross-thread representation integral and lock-free.
// The input owner still serializes writes to NativeMouseInput itself.
static _Atomic int64_t phase_yaw_nr, phase_pitch_nr;
static _Atomic uint64_t phase_generation;

void native_mouse_configure(NativeMouseInput *m, double sensitivity, bool invert_y) {
    m->sensitivity = isfinite(sensitivity) ? fmin(10.0, fmax(0.0, sensitivity)) : 1.0;
    m->invert_y = invert_y;
}

void native_mouse_clear(NativeMouseInput *m, uint64_t now_ns) {
    m->dx = m->dy = 0;
    m->held = m->pressed = m->moved_ns = 0;
    m->sample = (NativeMouseSample){0};
    m->sampled_ns = now_ns;
    m->has_frame = false;
    m->has_camera = m->stick_override = false;
}

void native_mouse_capture(NativeMouseInput *m, bool captured, uint64_t now_ns) {
    native_mouse_clear(m, now_ns);
    m->captured = captured;
}

bool native_mouse_motion(NativeMouseInput *m, double dx, double dy, uint64_t now_ns) {
    if (!m->captured || !m->sensitivity || !isfinite(dx) || !isfinite(dy) || (!dx && !dy)) return false;
    // Bound accumulated input after a stall; never replay a long queue of turns.
    m->dx = fmax(-1000000.0, fmin(1000000.0, m->dx + dx));
    m->dy = fmax(-1000000.0, fmin(1000000.0, m->dy + dy));
    m->moved_ns = now_ns;
    double scale = m->sensitivity * (0.08 * 3.14159265358979323846 / 180.0);
    atomic_fetch_add_explicit(&phase_yaw_nr, (int64_t)llround(dx * scale * 1e9), memory_order_relaxed);
    atomic_fetch_add_explicit(&phase_pitch_nr,
                              (int64_t)llround(dy * scale * (m->invert_y ? -1 : 1) * 1e9),
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&phase_generation, 1, memory_order_release);
    return true;
}

NativeMousePhase runtime_input_mouse_phase(void) {
    uint64_t before, after;
    int64_t yaw, pitch;
    do {
        before = atomic_load_explicit(&phase_generation, memory_order_acquire);
        yaw = atomic_load_explicit(&phase_yaw_nr, memory_order_relaxed);
        pitch = atomic_load_explicit(&phase_pitch_nr, memory_order_relaxed);
        after = atomic_load_explicit(&phase_generation, memory_order_acquire);
    } while (before != after);
    return (NativeMousePhase){yaw * 1e-9, pitch * 1e-9, after};
}

bool native_mouse_button(NativeMouseInput *m, unsigned button, bool down) {
    // Left: attack; right: shield/target; middle: bow; side: interact/cancel.
    uint64_t bit = native_mouse_button_mapping(button);
    if (!m->captured || !bit) return false;
    if (((m->held & bit) != 0) == down) return false;
    if (down) { m->held |= bit; m->pressed |= bit; }
    else m->held &= ~bit;
    return true;
}

static int32_t stick(double value) {
    return (int32_t)lround(fmax(-1.0, fmin(1.0, value)) * 32767.0);
}

void native_mouse_camera(NativeMouseInput *m, uint64_t frame, bool active) {
    m->has_camera = active;
    m->camera_frame = frame;
}

NativeMouseTurn native_mouse_turn(NativeMouseInput *m, uint64_t now_ns) {
    NativeMouseTurn turn = {0};
    if (m->stick_override) return turn;
    if (m->direct && m->captured && now_ns >= m->moved_ns && now_ns - m->moved_ns <= 100000000ULL) {
        // Angular displacement follows device displacement, independent of FPS.
        double scale = m->sensitivity * (0.08 * 3.14159265358979323846 / 180.0);
        turn.yaw = m->dx * scale;
        turn.pitch = m->dy * scale * (m->invert_y ? -1 : 1);
    }
    m->dx = m->dy = 0;
    return turn;
}

NativeMouseSample native_mouse_sample(NativeMouseInput *m, uint64_t frame, uint64_t now_ns) {
    if (!m->captured) return (NativeMouseSample){0};
    if (m->has_frame && frame == m->frame) return m->sample;
    m->frame = frame;
    m->has_frame = true;
    double seconds = now_ns > m->sampled_ns ? (now_ns - m->sampled_ns) * 1e-9 : 1.0 / 144.0;
    seconds = fmax(0.001, seconds);
    double scale = m->sensitivity / (3000.0 * seconds);
    // Menus and other camera owners still consume the game's right stick.
    // A normal-camera heartbeat reserves deltas only while that owner runs.
    bool direct = m->direct && m->has_camera && !m->stick_override &&
        frame >= m->camera_frame && frame - m->camera_frame <= 1;
    if (direct || now_ns < m->moved_ns || now_ns - m->moved_ns > 100000000ULL) scale = 0;
    m->sample = (NativeMouseSample){m->held | m->pressed,
        stick(m->dx * scale), stick(m->dy * scale * (m->invert_y ? -1 : 1))};
    if (!direct) m->dx = m->dy = 0;
    m->pressed = 0;
    m->sampled_ns = now_ns;
    return m->sample;
}
