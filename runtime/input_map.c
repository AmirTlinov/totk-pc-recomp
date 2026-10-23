// Binding table and the pure mapping functions (input_map.h). No Win32 or SDL here.
#include <math.h>

#include "input_map.h"

#define STICK_MAX 32767
#define DIAGONAL 23170   // STICK_MAX / sqrt(2)
#define MOUSE_FULL_CPS 3000.0

const Binding input_bindings[] = {
    {KEY_W, B_SL_UP}, {KEY_A, B_SL_LEFT}, {KEY_S, B_SL_DOWN}, {KEY_D, B_SL_RIGHT},                  // move
    {KEY_UP, B_SR_UP}, {KEY_LEFT, B_SR_LEFT}, {KEY_DOWN, B_SR_DOWN}, {KEY_RIGHT, B_SR_RIGHT},       // camera, second source
    {KEY_MOUSE_L, B_Y}, {KEY_MOUSE_R, B_ZL}, {KEY_Z, B_ZL}, {KEY_F, B_ZR}, {KEY_X, B_ZR},
    {KEY_SPACE, B_X}, {KEY_LSHIFT, B_B}, {KEY_E, B_A}, {KEY_Q, B_L}, {KEY_R, B_R},
    {KEY_LCTRL, B_STICK_L}, {KEY_C, B_STICK_R},
    {KEY_1, B_LEFT}, {KEY_2, B_UP}, {KEY_3, B_RIGHT}, {KEY_4, B_DOWN},
    {KEY_TAB, B_PLUS}, {KEY_M, B_MINUS},
};
const int input_binding_count = (int)(sizeof input_bindings / sizeof *input_bindings);

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
