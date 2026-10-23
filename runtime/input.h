// Host input sampling (runtime/input_win.c): SDL3 gamepad and the keyboard as one Switch npad.
#pragma once
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#endif

// Bits of libnx HidNpadButton (A=0 B=1 X=2 Y=3 StickL=4 StickR=5 L=6 R=7 ZL=8 ZR=9 Plus=10 Minus=11 Left=12 Up=13 Right=14
// Down=15) and the stick directions (StickLLeft..Down 16-19, StickRLeft..Down 20-23, derived from the sticks).
// Sticks are -32767..32767, up is +y. TOTK_INPUT=none reports no input. Callable from any thread.
void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
// The same for host UI (the text entry): never blocked. input_block(1) while a text entry is open: input_poll then
// reports nothing; input_block(0) when it closes: nothing until all buttons and stick directions are released.
void input_poll_ui(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
void input_block(int on);

// Mouse camera and cursor (input_win.c, Win32 window thread). The window procedure calls input_window_msg first for raw
// mouse input, cursor hiding and confinement; it returns 1 with *res set when the message is handled. input_cursor_suspend(1)
// releases the cursor around an F11 window change, (0) captures it again once the client area has its new size.
#ifdef _WIN32
int input_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res);
#endif
void input_cursor_suspend(int on);

// Rumble: SDL_RumbleGamepad on the open gamepad, low (heavy, left) and high (light, right) motor 0..65535 for ms
// milliseconds. 1 if a gamepad with rumble took it. Callable from any thread.
int input_rumble(uint16_t low, uint16_t high, uint32_t ms);
// Motor value for a normalised amplitude: pow(a, curve) * strength, saturated to 0..65535. Pure.
uint16_t rumble_motor(float a, float curve, float strength);
// TOTK_RUMBLE_TRIGGERS=1: input_rumble also drives the impulse triggers (Xbox) with the high motor's value.
int input_rumble_triggers_enabled(void);

// Motion (TOTK_MOTION=0 turns it off). input_motion_start starts the sensor thread once: every 4 ms it reads the
// gamepad's gyro and accelerometer (when it has them) and feeds motion_global(). Callable from any thread.
void input_motion_start(void);
// SDL sensor values (accelerometer m/s^2, gyro rad/s, SDL gamepad axes) to the Switch controller frame motion_feed
// takes (acceleration in G as gravity reads, rotation rad/s). The mapping is documented in input_win.c.
void input_motion_axes(const float sdl_acc[3], const float sdl_gyro[3], double acc_g[3], double gyro[3]);

// motion_fusion.c: nn::hid::SixAxisSensorState and the sensor fusion that produces it.
typedef struct {
    int64_t delta_ns, sampling;
    float acc[3], gyro[3], angle[3], dir[9];   // G, revolutions/s, revolutions, controller axes in the reference frame
    uint32_t attributes, reserved;             // bit 0 connected
} MotionState;
_Static_assert(sizeof(MotionState) == 0x60, "SixAxisSensorState ABI");
typedef struct MotionFusion MotionFusion;
MotionFusion *motion_new(void);
MotionFusion *motion_global(void);   // the gamepad's, fed by input_win.c
void motion_feed(MotionFusion *m, const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);
int motion_read(MotionFusion *m, MotionState *out, int max, uint64_t now_ns);   // newest first; 0 = none in 200 ms
void motion_restart(MotionFusion *m);   // angle and direction start over at the next sample
