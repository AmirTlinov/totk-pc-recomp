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

// Gamepad inputs by position (Nintendo layout, as SDL names them), platform-neutral like the key ids. ZL and ZR are the
// analog triggers; input_win.c translates SDL buttons and axes to these (one place).
enum {
    PAD_NONE = 0,
    PAD_SOUTH, PAD_EAST, PAD_WEST, PAD_NORTH, PAD_LSTICK, PAD_RSTICK, PAD_L, PAD_R, PAD_START, PAD_BACK,
    PAD_DPAD_LEFT, PAD_DPAD_UP, PAD_DPAD_RIGHT, PAD_DPAD_DOWN, PAD_ZL, PAD_ZR,
    PAD_COUNT
};

enum { INPUT_BINDING_MAX = 96, INPUT_PAD_BINDING_MAX = 48 };

// The tables a remapping page edits. Defaults are the layout in docs/controls.md; input_map_reset restores them.
typedef struct { uint8_t key; uint8_t action; } Binding;
typedef struct { uint8_t pad; uint8_t action; } PadBinding;
extern Binding input_bindings[INPUT_BINDING_MAX];
extern int input_binding_count;
extern PadBinding input_pad_bindings[INPUT_PAD_BINDING_MAX];
extern int input_pad_binding_count;

void input_map_reset(void);
// Replaces every binding of the Switch button `button` (script name: A B X Y LCLICK RCLICK L R ZL ZR PLUS MINUS LEFT UP
// RIGHT DOWN, any case) with `list`: comma separated "pad:<name>" and "key:<name>" entries, or "none" for no input.
// Invalid entries are logged and skipped; when none is valid the button keeps its bindings. 1 when applied.
int input_map_bind(const char *button, const char *list);
// Applies the TOTK_BIND_<button> settings (cfg_get) over the defaults. Once, at start.
void input_map_configure(void);
// down[pad] != 0 for the gamepad inputs held. Returns the Switch button bits (0..15) the pad table presses.
uint64_t input_map_pad_buttons(const uint8_t down[PAD_COUNT]);

// down[key] != 0 for the keys held. Returns the Switch button bits (0..15) the table presses.
uint64_t input_map_buttons(const uint8_t down[KEY_COUNT]);
// out = x, y of the stick (0 left, 1 right) from its four direction actions; up is +y; a diagonal is scaled to the
// unit circle.
void input_map_stick(const uint8_t down[KEY_COUNT], int stick, int32_t out[2]);
// Mouse motion in raw counts over dt_s seconds to right-stick deflection: full deflection at 3000 / sens counts/s.
// Clamped to the unit circle, up (negative dy) is +y.
void input_mouse_stick(int32_t dx, int32_t dy, double dt_s, float sens, int32_t out[2]);
