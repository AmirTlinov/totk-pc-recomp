// Keyboard and mouse bindings, free of any window system: the table maps platform-neutral key ids to Switch actions.
// input_win.c translates Win32 virtual keys to key ids (one place); a Linux port translates SDL scancodes the same way.
// A remapping page can later edit the table at run time. Layout in docs/controls.md.
#pragma once
#include <stdint.h>

// Actions are the bit numbers of libnx HidNpadButton (input.h); 16..23 are the stick directions, which keys drive as
// stick deflection.
enum {
    B_A = 0, B_B, B_X, B_Y, B_STICK_L, B_STICK_R, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS, B_LEFT, B_UP, B_RIGHT, B_DOWN,
    B_SL_LEFT = 16, B_SL_UP, B_SL_RIGHT, B_SL_DOWN, B_SR_LEFT, B_SR_UP, B_SR_RIGHT, B_SR_DOWN,
    ACTION_COUNT = 24
};

enum {
    KEY_NONE = 0,
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,   // letters and digits are
    KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,   // consecutive
    KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    KEY_SPACE, KEY_TAB, KEY_LSHIFT, KEY_LCTRL, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_MOUSE_L, KEY_MOUSE_R, KEY_MOUSE_M,
    KEY_COUNT
};

typedef struct { uint8_t key; uint8_t action; } Binding;
extern const Binding input_bindings[];
extern const int input_binding_count;

// down[key] != 0 for the keys held. Returns the Switch button bits (0..15) the table presses.
uint64_t input_map_buttons(const uint8_t down[KEY_COUNT]);
// out = x, y of the stick (0 left, 1 right) from its four direction actions; up is +y; a diagonal is scaled to the
// unit circle.
void input_map_stick(const uint8_t down[KEY_COUNT], int stick, int32_t out[2]);
// Mouse motion in raw counts over dt_s seconds to right-stick deflection: full deflection at 3000 / sens counts/s.
// Clamped to the unit circle, up (negative dy) is +y.
void input_mouse_stick(int32_t dx, int32_t dy, double dt_s, float sens, int32_t out[2]);
