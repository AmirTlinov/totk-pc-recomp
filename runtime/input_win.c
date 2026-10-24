// Host input for nn::hid (runtime/ov_input.c): an SDL3 gamepad (no SDL window, no SDL video), the keyboard and the mouse,
// merged into Switch npad button bits and stick axes. Button order follows libnx HidNpadButton (ISC). The key and pad bindings
// are the tables in input_map.c (totk.ini overrides them); this file translates Win32 virtual keys to its key ids, reads the mouse (raw input, WM_INPUT)
// and owns the cursor capture. Keys and their map: docs/controls.md.
// The gamepad also rumbles (input_rumble) and, when it has a gyro and an accelerometer, feeds the six-axis sensor from a
// thread of its own (input_motion_start). Every SDL call on the gamepad is made under `lock`.
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "nvn_api.h"
#include "vk.h"

enum { STICK_MAX = 32767, STICK_DEAD = 2500, STICK_DIR = 16384, TRIGGER_ON = 16384, RESCAN_MS = 1000 };

static SRWLOCK lock = SRWLOCK_INIT;
static int state;                    // 0 not started, 1 running, -1 disabled or SDL failed
static SDL_Gamepad *pad;
static uint64_t next_scan;
static int pad_rumble;               // the open gamepad has rumble motors
static SDL_JoystickID sensor_pad;    // the gamepad whose sensors the motion thread set up, 0 none
static int sensor_ok;                // ... and it has both sensors, enabled
static int motion_thread;            // 0 not started, 1 running, -1 off (TOTK_MOTION=0, no SDL)

// SDL buttons of the gamepad inputs (input_map.h PAD_*), by position; PAD_ZL and PAD_ZR are the triggers (axes).
static const SDL_GamepadButton pad_sdl[PAD_COUNT] = {
    [PAD_SOUTH] = SDL_GAMEPAD_BUTTON_SOUTH, [PAD_EAST] = SDL_GAMEPAD_BUTTON_EAST, [PAD_WEST] = SDL_GAMEPAD_BUTTON_WEST,
    [PAD_NORTH] = SDL_GAMEPAD_BUTTON_NORTH, [PAD_LSTICK] = SDL_GAMEPAD_BUTTON_LEFT_STICK,
    [PAD_RSTICK] = SDL_GAMEPAD_BUTTON_RIGHT_STICK, [PAD_L] = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    [PAD_R] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, [PAD_START] = SDL_GAMEPAD_BUTTON_START, [PAD_BACK] = SDL_GAMEPAD_BUTTON_BACK,
    [PAD_DPAD_LEFT] = SDL_GAMEPAD_BUTTON_DPAD_LEFT, [PAD_DPAD_UP] = SDL_GAMEPAD_BUTTON_DPAD_UP,
    [PAD_DPAD_RIGHT] = SDL_GAMEPAD_BUTTON_DPAD_RIGHT, [PAD_DPAD_DOWN] = SDL_GAMEPAD_BUTTON_DPAD_DOWN,
};

// TOTK_INPUT_SCRIPT=<file>: lines "<frame> <button> [hold]" or "<frame> LSTICK|RSTICK <x> <y> [hold]", ORed into the real input.
typedef struct { uint64_t frame, hold; int bit, stick; int32_t x, y; int logged; char text[48]; } ScriptEv;
static const struct { const char *name; int bit; } script_buttons[] = {
    {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"LCLICK", B_STICK_L}, {"RCLICK", B_STICK_R}, {"L", B_L}, {"R", B_R},
    {"ZL", B_ZL}, {"ZR", B_ZR}, {"PLUS", B_PLUS}, {"MINUS", B_MINUS}, {"LEFT", B_LEFT}, {"UP", B_UP}, {"RIGHT", B_RIGHT}, {"DOWN", B_DOWN},
};
static ScriptEv *script_ev;
static int script_n, script_state;   // 0 not loaded, 1 loaded

static void script_load(void) {
    script_state = 1;
    const char *path = cfg_get("TOTK_INPUT_SCRIPT");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) { rt_log("input: cannot open TOTK_INPUT_SCRIPT %s", path); return; }
    char line[256];
    int cap = 0, ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char name[32];
        unsigned long long fr;
        int off = 0;
        if (sscanf(line, "%llu %31s%n", &fr, name, &off) < 2) continue;
        ScriptEv e = {.frame = fr, .hold = 4, .bit = -1};
        long a, b, c;
        if (!strcmp(name, "LSTICK") || !strcmp(name, "RSTICK")) {
            int n = sscanf(line + off, "%ld %ld %ld", &a, &b, &c);
            if (n < 2) { rt_log("input: script line %d: %s needs x y [hold]", ln, name); continue; }
            e.stick = name[0] == 'L' ? 1 : 2; e.x = (int32_t)a; e.y = (int32_t)b;
            if (n >= 3) e.hold = (uint64_t)c;
            snprintf(e.text, sizeof e.text, "%s %d %d", name, e.x, e.y);
        } else {
            for (size_t i = 0; i < sizeof script_buttons / sizeof *script_buttons; i++)
                if (!strcmp(name, script_buttons[i].name)) e.bit = script_buttons[i].bit;
            if (e.bit < 0) { rt_log("input: script line %d: unknown button %s", ln, name); continue; }
            if (sscanf(line + off, "%ld", &a) == 1) e.hold = (uint64_t)a;
            snprintf(e.text, sizeof e.text, "%s", name);
        }
        if (script_n == cap) {
            cap = cap ? cap * 2 : 32;
            script_ev = realloc(script_ev, (size_t)cap * sizeof *script_ev);
            if (!script_ev) { script_n = 0; fclose(f); return; }
        }
        script_ev[script_n++] = e;
    }
    fclose(f);
    rt_log("input: script %s, %d events", path, script_n);
}

static void script_apply(uint64_t *b, int32_t sl[2], int32_t sr[2]) {
    if (!script_state) script_load();
    if (!script_n) return;
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    for (int i = 0; i < script_n; i++) {
        ScriptEv *e = &script_ev[i];
        if (fr < e->frame || fr >= e->frame + e->hold) continue;
        if (!e->logged) { e->logged = 1; rt_log("input: script frame %llu: %s", (unsigned long long)fr, e->text); }
        if (e->stick) { int32_t *s = e->stick == 1 ? sl : sr; s[0] = e->x; s[1] = e->y; }
        else *b |= 1ull << e->bit;
    }
}

// TOTK_INPUT_RECORD=<file>: writes the merged input as TOTK_INPUT_SCRIPT lines, one per button press and per stick
// segment, so a played session replays as a script. A stick segment ends when an axis moves by more than REC_STICK_STEP.
enum { REC_STICK_STEP = 2048 };
static FILE *rec;
static int rec_state;                // 0 not opened, 1 recording, -1 off
static uint64_t rec_down[16];        // frame + 1 at which each button went down, 0 while up
static uint64_t rec_stick_from[2];   // frame + 1 at which the current non-zero stick segment began
static int32_t rec_stick[2][2];

static void rec_close_stick(int s, uint64_t fr) {
    if (!rec_stick_from[s]) return;
    uint64_t from = rec_stick_from[s] - 1;
    fprintf(rec, "%llu %s %d %d %llu\n", (unsigned long long)from, s ? "RSTICK" : "LSTICK", rec_stick[s][0],
            rec_stick[s][1], (unsigned long long)(fr > from ? fr - from : 1));
    rec_stick_from[s] = 0;
}

static void rec_flush_all(void) {
    if (rec_state != 1) return;
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    for (int i = 0; i < 16; i++)
        if (rec_down[i]) { fprintf(rec, "%llu %s %llu\n", (unsigned long long)(rec_down[i] - 1), script_buttons[i].name,
                                   (unsigned long long)(fr - (rec_down[i] - 1) + 1)); rec_down[i] = 0; }
    rec_close_stick(0, fr);
    rec_close_stick(1, fr);
    fflush(rec);
}

static void rec_apply(uint64_t b, const int32_t sl[2], const int32_t sr[2]) {
    if (!rec_state) {
        const char *path = cfg_get("TOTK_INPUT_RECORD");
        rec = path && *path ? fopen(path, "w") : NULL;
        rec_state = rec ? 1 : -1;
        if (path && *path && !rec) rt_log("input: cannot open TOTK_INPUT_RECORD %s", path);
        if (rec) { fprintf(rec, "# Recorded input (TOTK_INPUT_RECORD); frame = presents.\n"); atexit(rec_flush_all);
                   rt_log("input: recording to %s", path); }
    }
    if (rec_state != 1) return;
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    int wrote = 0;
    for (int i = 0; i < 16; i++) {
        int bit = script_buttons[i].bit, down = (b >> bit & 1) != 0;
        if (down && !rec_down[i]) rec_down[i] = fr + 1;
        else if (!down && rec_down[i]) {
            uint64_t from = rec_down[i] - 1;
            fprintf(rec, "%llu %s %llu\n", (unsigned long long)from, script_buttons[i].name,
                    (unsigned long long)(fr > from ? fr - from : 1));
            rec_down[i] = 0;
            wrote = 1;
        }
    }
    const int32_t *st[2] = {sl, sr};
    for (int s = 0; s < 2; s++) {
        int moved = abs(st[s][0] - rec_stick[s][0]) > REC_STICK_STEP || abs(st[s][1] - rec_stick[s][1]) > REC_STICK_STEP ||
                    ((st[s][0] || st[s][1]) != (rec_stick[s][0] || rec_stick[s][1]));
        if (!moved) continue;
        if (rec_stick_from[s]) { rec_close_stick(s, fr); wrote = 1; }
        rec_stick[s][0] = st[s][0]; rec_stick[s][1] = st[s][1];
        if (st[s][0] || st[s][1]) rec_stick_from[s] = fr + 1;
    }
    if (wrote) fflush(rec);
}

// Win32 virtual key of a key id (the one place that knows them), 0 for an id without a key.
static int key_vk(int key) {
    if (key >= KEY_A && key <= KEY_Z) return 'A' + (key - KEY_A);
    if (key >= KEY_0 && key <= KEY_9) return '0' + (key - KEY_0);
    switch (key) {
    case KEY_SPACE: return VK_SPACE;
    case KEY_TAB: return VK_TAB;
    case KEY_LSHIFT: return VK_LSHIFT;
    case KEY_LCTRL: return VK_LCONTROL;
    case KEY_UP: return VK_UP;
    case KEY_DOWN: return VK_DOWN;
    case KEY_LEFT: return VK_LEFT;
    case KEY_RIGHT: return VK_RIGHT;
    case KEY_MOUSE_L: return VK_LBUTTON;
    case KEY_MOUSE_R: return VK_RBUTTON;
    case KEY_MOUSE_M: return VK_MBUTTON;
    }
    return 0;
}

// ---- mouse camera: raw motion accumulated by the window thread, one stick sample per presented frame
static volatile LONG mouse_dx, mouse_dy;   // raw counts since the last frame sample
static volatile LONG cur_captured;         // cursor hidden and confined to the client area
static volatile LONG cur_active;           // WM_ACTIVATEAPP: the window's application is active
static volatile LONG cur_released;         // Esc, or activation by a click on the title bar or border: until a client click
static volatile LONG cur_suspend;          // F11 or a modal size or move loop under way
static SRWLOCK cur_lock = SRWLOCK_INIT;    // serialises cursor_update (window thread, input_block callers)
static volatile LONG g_block;              // text entry state, see input_block
static float mouse_sens = -1;              // TOTK_MOUSE_SENS, 0 = mouse camera off, read once

static float mouse_sens_get(void) {
    if (mouse_sens >= 0) return mouse_sens;
    const char *e = cfg_get("TOTK_MOUSE_SENS");
    char *end;
    double v = e ? strtod(e, &end) : 1;
    if (!e || end == e || *end || v != v) v = 1;
    mouse_sens = (float)(v < 0 ? 0 : v > 10 ? 10 : v);
    return mouse_sens;
}

// Hide the cursor and confine it to the client area while the window is focused, not suspended, not behind a text entry
// and the mouse camera is on; release it otherwise. Safe from any thread.
static void cursor_update(HWND h) {
    const char *in = cfg_get("TOTK_INPUT");
    AcquireSRWLockExclusive(&cur_lock);
    int want = h && cur_active && !cur_released && !cur_suspend && g_block != 1 && mouse_sens_get() > 0 && !(in && !strcmp(in, "none")) &&
               GetForegroundWindow() == h && !IsIconic(h);
    RECT r;
    POINT tl = {0, 0};
    if (want && GetClientRect(h, &r) && ClientToScreen(h, &tl)) {
        RECT clip = {tl.x, tl.y, tl.x + r.right, tl.y + r.bottom};
        ClipCursor(&clip);
        if (!cur_captured) { InterlockedExchange(&mouse_dx, 0); InterlockedExchange(&mouse_dy, 0); }
        InterlockedExchange(&cur_captured, 1);
    } else {
        if (cur_captured) ClipCursor(NULL);
        InterlockedExchange(&cur_captured, 0);
    }
    ReleaseSRWLockExclusive(&cur_lock);
}

int input_cursor_released(void) { return cur_released != 0; }

void input_cursor_suspend(int on) {
    InterlockedExchange(&cur_suspend, on != 0);
    cursor_update(gfx_window_handle());
}

// Window procedure hook (gfx_window.c calls it first). Returns 1 with *res set when the message is fully handled.
int input_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res) {
    switch (m) {
    case WM_CREATE: {
        RAWINPUTDEVICE rid = {.usUsagePage = 1, .usUsage = 2, .dwFlags = 0, .hwndTarget = h};   // generic mouse
        if (!RegisterRawInputDevices(&rid, 1, sizeof rid)) rt_log("input: raw mouse registration failed (%lu)", GetLastError());
        break;
    }
    case WM_INPUT: {
        RAWINPUT ri;
        UINT sz = sizeof ri;
        if (cur_captured && GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            InterlockedAdd(&mouse_dx, ri.data.mouse.lLastX);
            InterlockedAdd(&mouse_dy, ri.data.mouse.lLastY);
        }
        break;   // DefWindowProc must see WM_INPUT for cleanup
    }
    case WM_SETCURSOR:
        if (cur_captured && LOWORD(l) == HTCLIENT) { SetCursor(NULL); *res = TRUE; return 1; }
        break;
    case WM_KEYDOWN:   // Esc is unbound in the game layout, so it only releases the cursor
        if (w == VK_ESCAPE) { InterlockedExchange(&cur_released, 1); cursor_update(h); }
        break;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:   // client area only; poll_merged drops the click
        if (cur_released) { InterlockedExchange(&cur_released, 0); cursor_update(h); }
        break;
    case WM_MOUSEACTIVATE:   // sent before WM_ACTIVATEAPP: a title bar or border click must not capture
        if (LOWORD(l) != HTCLIENT) InterlockedExchange(&cur_released, 1);
        break;
    case WM_ACTIVATEAPP:
        if (!w) InterlockedExchange(&cur_released, 0);   // Alt+Tab back in captures again
        InterlockedExchange(&cur_active, w != 0);
        cursor_update(h);
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_SIZE: case WM_MOVE:
        cursor_update(h);
        break;
    case WM_ENTERSIZEMOVE:
        input_cursor_suspend(1);
        break;
    case WM_EXITSIZEMOVE:
        input_cursor_suspend(0);
        break;
    case WM_CLOSE:
        InterlockedExchange(&cur_active, 0);
        cursor_update(h);
        break;
    }
    return 0;
}

static int key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// The mouse camera as a right-stick sample, held for the whole presented frame. Under `lock`.
static void mouse_camera(int32_t out[2]) {
    static uint64_t frame = ~0ull;
    static LARGE_INTEGER t0;
    static int32_t held[2];
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    if (!cur_captured) {
        InterlockedExchange(&mouse_dx, 0);
        InterlockedExchange(&mouse_dy, 0);
        held[0] = held[1] = 0;
        frame = ~0ull;
    } else {
        uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        double age = frame == ~0ull ? 0 : (double)(now.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        if (fr != frame) {
            double dt = age < 0.004 ? 0.004 : age > 0.1 ? 0.1 : age;
            input_mouse_stick(InterlockedExchange(&mouse_dx, 0), InterlockedExchange(&mouse_dy, 0), dt, mouse_sens_get(), held);
            frame = fr;
            t0 = now;
        } else if (age > 0.1) {
            held[0] = held[1] = 0;   // frames stopped: do not hold a deflection
        }
    }
    out[0] = held[0];
    out[1] = held[1];
}

static int32_t axis(int16_t v) {
    int32_t a = v < -STICK_MAX ? -STICK_MAX : v;
    return a > -STICK_DEAD && a < STICK_DEAD ? 0 : a;
}

// Open the first gamepad when none is open or the open one went away; logs once per change.
static void scan_pads(void) {
    if (pad && SDL_GamepadConnected(pad)) return;
    if (pad) { SDL_CloseGamepad(pad); pad = NULL; sensor_pad = 0; sensor_ok = 0; rt_log("input: gamepad disconnected"); }
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n && !pad; i++) pad = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
    if (!pad) return;
    pad_rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    int motion = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL);
    rt_log("input: gamepad \"%s\" (%s, %s)", SDL_GetGamepadName(pad), pad_rumble ? "rumble" : "no rumble",
           motion ? "gyro and accelerometer" : "no motion sensors");
}

static void start(void) {
    input_map_configure();
    const char *e = cfg_get("TOTK_INPUT");
    if (e && !strcmp(e, "none")) { state = -1; rt_log("input: disabled (TOTK_INPUT=none)"); return; }
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { state = -1; rt_log("input: SDL gamepad init failed (%s), keyboard only", SDL_GetError()); }
    else state = 1;
    if (state == 1) {
        scan_pads();
        if (!pad) rt_log("input: no gamepad (keyboard only until one is connected)");
    }
    if (state == -1) state = 2;   // keyboard only
}

static void stick_bits(uint64_t *b, int32_t x, int32_t y, int first) {
    if (x < -STICK_DIR) *b |= 1ull << (first + 0);
    if (y > STICK_DIR) *b |= 1ull << (first + 1);
    if (x > STICK_DIR) *b |= 1ull << (first + 2);
    if (y < -STICK_DIR) *b |= 1ull << (first + 3);
}

static void pick(int32_t *x, int32_t *y, int32_t kx, int32_t ky) {
    int64_t a = (int64_t)*x * *x + (int64_t)*y * *y, b = (int64_t)kx * kx + (int64_t)ky * ky;
    if (b > a) { *x = kx; *y = ky; }
}

static void poll_merged(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    uint64_t b = 0;
    int32_t sl[2] = {0, 0}, sr[2] = {0, 0};
    AcquireSRWLockExclusive(&lock);
    if (!state) start();
    if (state > 0) {
        if (state == 1) {
            if ((!pad || !SDL_GamepadConnected(pad)) && GetTickCount64() >= next_scan) { next_scan = GetTickCount64() + RESCAN_MS; scan_pads(); }
            SDL_UpdateGamepads();
            if (pad && SDL_GamepadConnected(pad)) {
                uint8_t pd[PAD_COUNT] = {0};
                for (int p = PAD_SOUTH; p < PAD_ZL; p++) pd[p] = SDL_GetGamepadButton(pad, pad_sdl[p]);
                pd[PAD_ZL] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_ON;
                pd[PAD_ZR] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_ON;
                b |= input_map_pad_buttons(pd);
                sl[0] = axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
                sl[1] = -axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
                sr[0] = axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX));
                sr[1] = -axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
            }
        }
        HWND w = gfx_window_handle();
        if (w && w == GetForegroundWindow()) {
            uint8_t down[KEY_COUNT] = {0};
            // Mouse buttons count only while the cursor is captured, and a button already held when the capture began
            // (the click that refocused the window) is ignored until it is released.
            static int was_captured;
            static uint8_t mouse_ignore[KEY_COUNT];
            int cap = cur_captured != 0;
            for (int k = 1; k < KEY_COUNT; k++) {
                int vk = key_vk(k);
                down[k] = vk && key_down(vk);
                if (k < KEY_MOUSE_L || k > KEY_MOUSE_M) continue;
                if (cap && !was_captured) mouse_ignore[k] = down[k];
                if (!down[k]) mouse_ignore[k] = 0;
                if (!cap || mouse_ignore[k]) down[k] = 0;
            }
            was_captured = cap;
            b |= input_map_buttons(down);
            int32_t k[2];
            input_map_stick(down, 0, k);
            pick(&sl[0], &sl[1], k[0], k[1]);
            input_map_stick(down, 1, k);
            pick(&sr[0], &sr[1], k[0], k[1]);
            mouse_camera(k);
            pick(&sr[0], &sr[1], k[0], k[1]);
        }
    }
    script_apply(&b, sl, sr);
    rec_apply(b, sl, sr);
    ReleaseSRWLockExclusive(&lock);
    stick_bits(&b, sl[0], sl[1], B_SL_LEFT);
    stick_bits(&b, sr[0], sr[1], B_SR_LEFT);
    *buttons = b; *lx = sl[0]; *ly = sl[1]; *rx = sr[0]; *ry = sr[1];
}

// Text entry (swkbd_win.c): while it is open the game reads no input, as behind the keyboard applet; after it closes
// the game reads none until every button and stick direction is released, so the press that closed it stays there.
// g_block (above): 0 off, 1 text entry open, 2 closed, waiting for the release. The cursor is released while it is open.
void input_block(int on) {
    InterlockedExchange(&g_block, on ? 1 : 2);
    cursor_update(gfx_window_handle());
}

void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    poll_merged(buttons, lx, ly, rx, ry);
    LONG k = g_block;
    if (!k || (k == 2 && !*buttons && InterlockedCompareExchange(&g_block, 0, 2) == 2)) return;
    *buttons = 0; *lx = *ly = *rx = *ry = 0;
}

void input_poll_ui(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    poll_merged(buttons, lx, ly, rx, ry);
}

uint16_t rumble_motor(float a, float curve, float strength) {
    if (!(a > 0) || !(strength > 0)) return 0;
    float v = powf(a > 1 ? 1 : a, curve) * strength * 65535.0f + 0.5f;
    return v >= 65535.0f ? 65535 : (uint16_t)v;
}

int input_rumble_triggers_enabled(void) {
    const char *e = cfg_get("TOTK_RUMBLE_TRIGGERS");
    return e && !strcmp(e, "1");
}

int input_rumble(uint16_t low, uint16_t high, uint32_t ms) {
    int ok = 0;
    static int triggers = -1;
    if (triggers < 0) triggers = input_rumble_triggers_enabled();
    AcquireSRWLockExclusive(&lock);
    if (state == 1 && pad && pad_rumble) {
        ok = SDL_RumbleGamepad(pad, low, high, ms);
        if (triggers) SDL_RumbleGamepadTriggers(pad, high, high, ms);   // unsupported pads return false: ignored
    }
    ReleaseSRWLockExclusive(&lock);
    return ok;
}

// SDL gamepad sensor axes (SDL_sensor.h, a gamepad held in front of you): x to the right, y up out of the face (lying
// flat face up, +y points at the ceiling), z toward the player (out of the bottom edge). The accelerometer reads the
// push against gravity in m/s^2 (lying flat: y = +9.81); the gyro reads rad/s, counter-clockwise positive (right hand).
// Switch six-axis axes (nn::hid, as MacNativeGames' GCMotion feed and its Pro Controller HID path use them): x to the
// right, y toward the top edge (away from the player), z out of the face; acceleration as gravity reads (lying flat:
// z = -1 G). So Switch x = SDL x, Switch y = -SDL z, Switch z = SDL y: a rotation (right-handed to right-handed), so the
// gyro keeps its sense, while the acceleration is also negated (push -> gravity) and scaled to G.
void input_motion_axes(const float a[3], const float g[3], double acc[3], double gyro[3]) {
    acc[0] = -a[0] / SDL_STANDARD_GRAVITY;
    acc[1] = a[2] / SDL_STANDARD_GRAVITY;
    acc[2] = -a[1] / SDL_STANDARD_GRAVITY;
    gyro[0] = g[0];
    gyro[1] = -g[2];
    gyro[2] = g[1];
}

// Under lock: (re)enable the sensors when the open gamepad changed; 1 if it has both and they are on.
static int sensors_ready(void) {
    SDL_JoystickID id = SDL_GetGamepadID(pad);
    if (id == sensor_pad) return sensor_ok;
    sensor_pad = id;
    // Nothing drains the SDL event queue: no event per sample (the data still updates, SDL_SendJoystickSensor)
    SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_SENSOR_UPDATE, false);
    sensor_ok =SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) &&
                SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true) &&
                SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
    if (sensor_ok) rt_log("input: motion sensors on (gyro %.0f Hz, accelerometer %.0f Hz)",
                          (double)SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_GYRO),
                          (double)SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_ACCEL));
    else rt_log("input: \"%s\" has no gyro and accelerometer: the six-axis sensor reads a controller at rest",
                SDL_GetGamepadName(pad));
    return sensor_ok;
}

// 250 Hz: the gamepads update (reports read, rumble expiry) and the newest gyro and accelerometer sample goes into the
// fusion. Without a gamepad with sensors nothing is fed and the six-axis sensor reads at rest.
static DWORD WINAPI motion_main(void *arg) {
    (void)arg;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (;;) {
        if (timer) {
            LARGE_INTEGER due = {.QuadPart = -40000};   // 4 ms
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else Sleep(4);
        float a[3], g[3];
        int have = 0;
        AcquireSRWLockExclusive(&lock);
        if (pad && SDL_GamepadConnected(pad)) {
            SDL_UpdateGamepads();
            have = sensors_ready() && SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, a, 3) &&
                   SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, g, 3);
        }
        ReleaseSRWLockExclusive(&lock);
        if (!have) continue;
        double acc[3], gyro[3];
        input_motion_axes(a, g, acc, gyro);
        motion_feed(motion_global(), acc, gyro, kern_now_ns());
    }
    return 0;
}

void input_motion_start(void) {
    AcquireSRWLockExclusive(&lock);
    if (!state) start();
    if (!motion_thread) {
        const char *e = cfg_get("TOTK_MOTION");
        if (e && !strcmp(e, "0")) { motion_thread = -1; rt_log("input: motion off (TOTK_MOTION=0)"); }
        else if (state != 1) motion_thread = -1;
        else {
            HANDLE h = CreateThread(NULL, 0, motion_main, NULL, 0, NULL);
            if (!h) rt_fatal("input: motion thread CreateThread failed (error %lu)", GetLastError());
            CloseHandle(h);
            motion_thread = 1;
        }
    }
    ReleaseSRWLockExclusive(&lock);
}
