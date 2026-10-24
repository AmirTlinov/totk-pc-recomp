// Binding tables (keyboard/mouse and gamepad), their totk.ini overrides and the pure mapping functions (input_map.h).
// No Win32 or SDL here.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input_map.h"

#define STICK_MAX 32767
#define DIAGONAL 23170   // STICK_MAX / sqrt(2)
#define MOUSE_FULL_CPS 3000.0

#define DEFAULT_KEYS \
    {KEY_W, B_SL_UP}, {KEY_A, B_SL_LEFT}, {KEY_S, B_SL_DOWN}, {KEY_D, B_SL_RIGHT},                  /* move */ \
    {KEY_UP, B_SR_UP}, {KEY_LEFT, B_SR_LEFT}, {KEY_DOWN, B_SR_DOWN}, {KEY_RIGHT, B_SR_RIGHT},       /* camera, second source */ \
    {KEY_MOUSE_L, B_Y}, {KEY_MOUSE_R, B_ZL}, {KEY_Z, B_ZL}, {KEY_F, B_ZR}, {KEY_X, B_ZR}, \
    {KEY_SPACE, B_X}, {KEY_LSHIFT, B_B}, {KEY_E, B_A}, {KEY_Q, B_L}, {KEY_R, B_R}, \
    {KEY_LCTRL, B_STICK_L}, {KEY_C, B_STICK_R}, \
    {KEY_1, B_LEFT}, {KEY_2, B_UP}, {KEY_3, B_RIGHT}, {KEY_4, B_DOWN}, \
    {KEY_TAB, B_PLUS}, {KEY_M, B_MINUS}

// By position: south is Switch B, east A, west Y, north X (Nintendo layout); the triggers are ZL and ZR.
#define DEFAULT_PADS \
    {PAD_SOUTH, B_B}, {PAD_EAST, B_A}, {PAD_WEST, B_Y}, {PAD_NORTH, B_X}, {PAD_LSTICK, B_STICK_L}, {PAD_RSTICK, B_STICK_R}, \
    {PAD_L, B_L}, {PAD_R, B_R}, {PAD_START, B_PLUS}, {PAD_BACK, B_MINUS}, {PAD_DPAD_LEFT, B_LEFT}, {PAD_DPAD_UP, B_UP}, \
    {PAD_DPAD_RIGHT, B_RIGHT}, {PAD_DPAD_DOWN, B_DOWN}, {PAD_ZL, B_ZL}, {PAD_ZR, B_ZR}

static const Binding default_keys[] = {DEFAULT_KEYS};
static const PadBinding default_pads[] = {DEFAULT_PADS};
Binding input_bindings[INPUT_BINDING_MAX] = {DEFAULT_KEYS};
int input_binding_count = (int)(sizeof default_keys / sizeof *default_keys);
PadBinding input_pad_bindings[INPUT_PAD_BINDING_MAX] = {DEFAULT_PADS};
int input_pad_binding_count = (int)(sizeof default_pads / sizeof *default_pads);

static const char *const button_names[16] = {"A", "B", "X", "Y", "LCLICK", "RCLICK", "L", "R", "ZL", "ZR", "PLUS", "MINUS",
                                             "LEFT", "UP", "RIGHT", "DOWN"};   // indexed by action
static const char *const pad_names[PAD_COUNT] = {
    NULL, "south", "east", "west", "north", "lstick", "rstick", "l", "r", "start", "back",
    "dpad_left", "dpad_up", "dpad_right", "dpad_down", "zl", "zr"};
static const char *const key_names[KEY_COUNT] = {   // letters and digits are handled in key_id
    [KEY_SPACE] = "space", [KEY_TAB] = "tab", [KEY_LSHIFT] = "lshift", [KEY_LCTRL] = "lctrl", [KEY_UP] = "up",
    [KEY_DOWN] = "down", [KEY_LEFT] = "left", [KEY_RIGHT] = "right", [KEY_MOUSE_L] = "mouse_l", [KEY_MOUSE_R] = "mouse_r",
    [KEY_MOUSE_M] = "mouse_m"};

void input_map_reset(void) {
    memcpy(input_bindings, default_keys, sizeof default_keys);
    input_binding_count = (int)(sizeof default_keys / sizeof *default_keys);
    memcpy(input_pad_bindings, default_pads, sizeof default_pads);
    input_pad_binding_count = (int)(sizeof default_pads / sizeof *default_pads);
}

static int key_id(const char *name) {
    if (name[0] && !name[1]) {
        int c = tolower((unsigned char)name[0]);
        if (c >= 'a' && c <= 'z') return KEY_A + (c - 'a');
        if (c >= '0' && c <= '9') return KEY_0 + (c - '0');
    }
    for (int k = KEY_SPACE; k < KEY_COUNT; k++)
        if (key_names[k] && !_stricmp(key_names[k], name)) return k;
    return KEY_NONE;
}

static int pad_id(const char *name) {
    for (int p = PAD_SOUTH; p < PAD_COUNT; p++)
        if (!_stricmp(pad_names[p], name)) return p;
    return PAD_NONE;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

int input_map_bind(const char *button, const char *list) {
    int action = -1;
    for (int a = 0; a < 16; a++)
        if (!_stricmp(button_names[a], button)) action = a;
    if (action < 0) { rt_log("input: bind: unknown button '%s'", button); return 0; }
    Binding keys[INPUT_BINDING_MAX];
    PadBinding pads[INPUT_PAD_BINDING_MAX];
    int nk = 0, np = 0, none = 0, bad = 0;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *save = NULL, *tok = strtok_s(buf, ",", &save); tok; tok = strtok_s(NULL, ",", &save)) {
        tok = trim(tok);
        if (!_stricmp(tok, "none")) { none = 1; continue; }
        int ok = 0;
        if (!_strnicmp(tok, "pad:", 4)) {
            int p = pad_id(trim(tok + 4));
            if (p && np < INPUT_PAD_BINDING_MAX) pads[np++] = (PadBinding){(uint8_t)p, (uint8_t)action}, ok = 1;
        } else if (!_strnicmp(tok, "key:", 4)) {
            int k = key_id(trim(tok + 4));
            if (k && nk < INPUT_BINDING_MAX) keys[nk++] = (Binding){(uint8_t)k, (uint8_t)action}, ok = 1;
        }
        if (!ok) { bad++; rt_log("input: bind %s: ignoring '%s' (expected pad:<name> or key:<name>)", button, tok); }
    }
    if (!nk && !np && !none) { if (bad) rt_log("input: bind %s: no valid entry, keeping the defaults", button); return 0; }
    int w = 0;
    for (int i = 0; i < input_binding_count; i++)
        if (input_bindings[i].action != action) input_bindings[w++] = input_bindings[i];
    input_binding_count = w;
    for (int i = 0; i < nk && input_binding_count < INPUT_BINDING_MAX; i++) input_bindings[input_binding_count++] = keys[i];
    w = 0;
    for (int i = 0; i < input_pad_binding_count; i++)
        if (input_pad_bindings[i].action != action) input_pad_bindings[w++] = input_pad_bindings[i];
    input_pad_binding_count = w;
    for (int i = 0; i < np && input_pad_binding_count < INPUT_PAD_BINDING_MAX; i++)
        input_pad_bindings[input_pad_binding_count++] = pads[i];
    return 1;
}

void input_map_configure(void) {
    input_map_reset();
    for (int a = 0; a < 16; a++) {
        char var[32];
        snprintf(var, sizeof var, "TOTK_BIND_%s", button_names[a]);
        const char *v = cfg_get(var);
        if (v && input_map_bind(button_names[a], v)) rt_log("input: %s = %s", var + 5, v);
    }
}

uint64_t input_map_pad_buttons(const uint8_t down[PAD_COUNT]) {
    uint64_t b = 0;
    for (int i = 0; i < input_pad_binding_count; i++)
        if (down[input_pad_bindings[i].pad]) b |= 1ull << input_pad_bindings[i].action;
    return b;
}

uint64_t input_map_buttons(const uint8_t down[KEY_COUNT]) {
    uint64_t b = 0;
    for (int i = 0; i < input_binding_count; i++)
        if (input_bindings[i].action < B_SL_LEFT && down[input_bindings[i].key]) b |= 1ull << input_bindings[i].action;
    return b;
}

void input_map_stick(const uint8_t down[KEY_COUNT], int stick, int32_t out[2]) {
    int first = stick ? B_SR_LEFT : B_SL_LEFT, dir[4] = {0, 0, 0, 0};   // left, up, right, down
    for (int i = 0; i < input_binding_count; i++) {
        int a = input_bindings[i].action - first;
        if (a >= 0 && a < 4 && down[input_bindings[i].key]) dir[a] = 1;
    }
    int dx = dir[2] - dir[0], dy = dir[1] - dir[3];
    int32_t m = dx && dy ? DIAGONAL : STICK_MAX;
    out[0] = dx * m;
    out[1] = dy * m;
}

void input_mouse_stick(int32_t dx, int32_t dy, double dt_s, float sens, int32_t out[2]) {
    out[0] = out[1] = 0;
    if (!(dt_s > 0) || !(sens > 0)) return;
    double k = STICK_MAX * (double)sens / (MOUSE_FULL_CPS * dt_s);
    double x = dx * k, y = -dy * k, len = sqrt(x * x + y * y);
    if (len > STICK_MAX) { x *= STICK_MAX / len; y *= STICK_MAX / len; }
    out[0] = (int32_t)lrint(x);
    out[1] = (int32_t)lrint(y);
}
