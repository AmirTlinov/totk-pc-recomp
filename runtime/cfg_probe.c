// --probe-cfg: totk.ini parsing, the environment override and a missing file (cfg.c). Needs no game files.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "platform/platform.h"
#ifdef _WIN32
#include <windows.h>
#endif

static int fails;
#define CHECK(c, what) do { if (!(c)) { fails++; fprintf(stderr, "FAIL cfg: %s\n", what); } } while (0)
#define EQ(a, b) ((a) && !strcmp((a), (b)))

int probe_cfg(void) {
    char path[1024];
    if (!plat_exe_path(path, sizeof path)) return 1;
    char *cut = strrchr(path, '\\');
    if (!cut) return 1;
    strcpy(cut + 1, "cfg-probe.ini");
    static const char ini[] =
        "\xEF\xBB\xBF# comment\r\n"
        "; another comment\r\n"
        "[frame rate]\r\n"
        "  fps60 = 0  \r\n"
        "Realtime=0\r\n"
        "[Display]\r\n"
        "GPU_name = \"My GPU 1\"\r\n"
        "TOTK_Locale = de-DE\r\n"
        "lang =\r\n"
        "trace = a\r\n"
        "trace = b\r\n"
        "no_such_setting = 1\r\n"
        "no equals sign\r\n"
        "last = no newline at the end";
    _putenv_s("TOTK_LOCALE", "");   // an empty variable is how _putenv_s spells "remove" on Windows
    _putenv_s("TOTK_FPS60", "");
    CHECK(plat_file_write(path, ini, sizeof ini - 1), "write the probe ini");
    CHECK(cfg_load(path) == 1, "load the probe ini");
    CHECK(EQ(cfg_get("TOTK_FPS60"), "0"), "value read, whitespace trimmed, lower-case key");
    CHECK(EQ(cfg_get("TOTK_REALTIME"), "0"), "key without spaces, mixed case");
    CHECK(EQ(cfg_get("TOTK_GPU_NAME"), "My GPU 1"), "quoted value, section ignored");
    CHECK(EQ(cfg_get("TOTK_LOCALE"), "de-DE"), "TOTK_ prefix accepted in the file");
    CHECK(cfg_get("TOTK_LANG") == NULL, "empty value is unset");
    CHECK(EQ(cfg_get("TOTK_TRACE"), "b"), "later line wins");
    CHECK(cfg_get("TOTK_CLOTH_REALTIME") == NULL, "absent key is NULL");
    CHECK(cfg_get("TOTK_comment") == NULL, "comment is not a key");
    CHECK(EQ(cfg_get("TOTK_NO_SUCH_SETTING"), "1"), "unknown key is still readable");
    _putenv_s("TOTK_FPS60", "1");
    CHECK(EQ(cfg_get("TOTK_FPS60"), "1"), "environment overrides the file");
    _putenv_s("TOTK_FPS60", "");
    CHECK(EQ(cfg_get("TOTK_FPS60"), "0"), "file value returns when the variable is removed");
    plat_file_delete(path);
    CHECK(cfg_load(path) == 0, "missing file is not an error");
    CHECK(cfg_get("TOTK_FPS60") == NULL, "missing file leaves no keys");

    // cfg_set / cfg_save: only changed lines move, comments stay, a missing key goes under its section
    static const char before[] =
        "\xEF\xBB\xBF# my settings\r\n"
        "[frame rate]\r\n"
        "  FPS60 = 1\r\n"
        "; keep this\r\n"
        "\r\n"
        "[input]\r\n"
        "# Gamepad motion controls (0 = off) and vibration (0 = off).\r\n"
        "# motion = 1\r\n"
        "# rumble = 1\r\n"
        "rumble_curve = 0.6\r\n"
        "\r\n"
        "[files]\r\n"
        "romfs_mods = C:\\mods";
    static const char after[] =
        "\xEF\xBB\xBF# my settings\r\n"
        "[frame rate]\r\n"
        "  FPS60 = 0\r\n"
        "; keep this\r\n"
        "\r\n"
        "[input]\r\n"
        "# Gamepad motion controls (0 = off) and vibration (0 = off).\r\n"
        "# motion = 1\r\n"
        "motion = 0\r\n"
        "# rumble = 1\r\n"
        "rumble_curve = 0.6\r\n"
        "rumble_strength = 80\r\n"
        "\r\n"
        "[files]\r\n"
        "romfs_mods = C:\\mods\r\n"
        "\r\n"
        "[audio]\r\n"
        "volume = 40\r\n";
    _putenv_s("TOTK_MOTION", "");
    _putenv_s("TOTK_VOLUME", "");
    _putenv_s("TOTK_RUMBLE_STRENGTH", "");
    CHECK(plat_file_write(path, before, sizeof before - 1), "write the save ini");
    CHECK(cfg_load(path) == 1, "load the save ini");
    unsigned v0 = cfg_version();
    cfg_set("TOTK_FPS60", "0");
    cfg_set("TOTK_MOTION", "0");
    cfg_set("TOTK_RUMBLE_STRENGTH", "80");
    cfg_set("TOTK_VOLUME", "40");
    CHECK(cfg_version() == v0 + 4, "cfg_set bumps the version");
    CHECK(EQ(cfg_get("TOTK_FPS60"), "0") && EQ(cfg_get("TOTK_VOLUME"), "40"), "cfg_set values are read back at once");
    CHECK(cfg_save_to(path) == 1, "cfg_save_to writes");
    uint64_t got_n = 0;
    uint8_t *got = plat_file_read(path, &got_n);
    CHECK(got && got_n == sizeof after - 1 && !memcmp(got, after, got_n),
          "saved file: changed line replaced, comments kept, missing keys under their sections");
    if (got && (got_n != sizeof after - 1 || memcmp(got, after, got_n))) fprintf(stderr, "---- saved:\n%s\n----\n", got);
    free(got);
    CHECK(cfg_save_to(path) == 1, "a save with nothing changed");
    got = plat_file_read(path, &got_n);
    CHECK(got && got_n == sizeof after - 1 && !memcmp(got, after, got_n), "a save with nothing changed keeps the file");
    free(got);
    CHECK(cfg_load(path) == 1 && EQ(cfg_get("TOTK_FPS60"), "0") && EQ(cfg_get("TOTK_MOTION"), "0") &&
          EQ(cfg_get("TOTK_RUMBLE_STRENGTH"), "80") && EQ(cfg_get("TOTK_VOLUME"), "40") &&
          EQ(cfg_get("TOTK_ROMFS_MODS"), "C:\\mods"), "the saved file reads back");
    const char *held = cfg_get("TOTK_VOLUME");
    _putenv_s("TOTK_VOLUME", "70");
    cfg_set("TOTK_VOLUME", "90");
    CHECK(cfg_from_env("TOTK_VOLUME") && EQ(cfg_get("TOTK_VOLUME"), "70"), "environment wins over cfg_set");
    CHECK(EQ(held, "40"), "a pointer cfg_get handed out stays valid after cfg_set");
    _putenv_s("TOTK_VOLUME", "");
    CHECK(!cfg_from_env("TOTK_VOLUME") && EQ(cfg_get("TOTK_VOLUME"), "90"), "cfg_set value returns without the variable");
    plat_file_delete(path);
    CHECK(cfg_save_to(path) == 1, "cfg_save_to creates a missing file");
    got = plat_file_read(path, &got_n);
    CHECK(got && !strcmp((const char *)got, "[audio]\nvolume = 90\n"), "new file: the section and the key");
    free(got);

    // Defaults table: the menu's "default" marks and Reset
    const CfgMeta *m = cfg_meta("FPS60");
    CHECK(m && EQ(m->def, "1") && m->live, "meta: fps60 defaults to 1 and is live");
    m = cfg_meta("ASYNC_PREEMPT");
    CHECK(m && EQ(m->values, "0|1|2") && !m->live, "meta: async_preempt takes 0|1|2 and needs a restart");
    m = cfg_meta("totk_vk_present");
    CHECK(m && EQ(m->key, "VK_PRESENT") && EQ(m->values, "mailbox|fifo|immediate") && m->live,
          "meta: prefix and case are accepted");
    m = cfg_meta("RUMBLE_STRENGTH");
    CHECK(m && EQ(m->def, "130") && EQ(m->values, "0..300"), "meta: rumble strength 130 in 0..300");
    m = cfg_meta("PERF_OVERLAY");
    CHECK(m && EQ(m->def, "off") && EQ(m->values, "off|fps|full") && m->live, "meta: perf_overlay is live, off by default");
    CHECK(cfg_meta("BIND_A") == NULL && cfg_meta("FPS_COUNTER") == NULL && cfg_meta("NO_SUCH") == NULL,
          "meta: keys the menu does not show are NULL");

    // cfg_reset: the key's active lines leave totk.ini, the commented example stays
    static const char rbase[] = "[audio]\n# volume = 100\n[input]\nmotion = 0\n";
    static const char *const vol[] = {"VOLUME"};
    _putenv_s("TOTK_VOLUME", "");
    CHECK(plat_file_write(path, rbase, sizeof rbase - 1), "write the reset ini");
    CHECK(cfg_load(path) == 1, "load the reset ini");
    cfg_set("TOTK_VOLUME", "40");
    CHECK(cfg_save_to(path) == 1 && cfg_last_save_ok(), "reset: volume saved");
    got = plat_file_read(path, &got_n);
    CHECK(got && strstr((const char *)got, "\nvolume = 40\n"), "reset: the active line is in the file");
    free(got);
    unsigned v1 = cfg_version();
    CHECK(cfg_reset(vol, 1) == 1, "reset: one key reset");
    CHECK(cfg_version() > v1 && cfg_get("TOTK_VOLUME") == NULL, "reset: the value is gone in memory, the version moved");
    CHECK(cfg_reset(vol, 1) == 0, "reset: a key already reset counts 0");
    CHECK(cfg_save_to(path) == 1, "reset: save");
    got = plat_file_read(path, &got_n);
    CHECK(got && got_n == sizeof rbase - 1 && !memcmp(got, rbase, got_n) && strstr((const char *)got, "# volume = 100"),
          "reset: no active volume line, the commented example and the other keys stay");
    if (got && (got_n != sizeof rbase - 1 || memcmp(got, rbase, got_n))) fprintf(stderr, "---- saved:\n%s\n----\n", got);
    free(got);
    cfg_set("TOTK_VOLUME", "55");
    CHECK(cfg_reset(vol, 1) == 1 && cfg_get("TOTK_VOLUME") == NULL, "reset: a value set after a save resets too");
    cfg_set("TOTK_VOLUME", "60");
    CHECK(cfg_save_to(path) == 1 && EQ(cfg_get("TOTK_VOLUME"), "60"), "reset then set before the save: the set wins");
    got = plat_file_read(path, &got_n);
    CHECK(got && strstr((const char *)got, "\nvolume = 60\n"), "reset then set: the new line is written");
    free(got);

    // a key the environment sets is skipped and keeps its file line
    CHECK(cfg_load(path) == 1 && EQ(cfg_get("TOTK_VOLUME"), "60"), "reset: reload");
    _putenv_s("TOTK_VOLUME", "70");
    CHECK(cfg_reset(vol, 1) == 0, "reset: an environment-set key is skipped");
    _putenv_s("TOTK_VOLUME", "");
    CHECK(EQ(cfg_get("TOTK_VOLUME"), "60"), "reset: the skipped key keeps its value");
    CHECK(cfg_save_to(path) == 1, "reset: save after a skip");
    got = plat_file_read(path, &got_n);
    CHECK(got && strstr((const char *)got, "\nvolume = 60\n"), "reset: the skipped key keeps its file line");
    free(got);

    // a failed save: the call fails, cfg_last_save_ok says so, the value stays set and a later save works
    CHECK(cfg_last_save_ok() == 1, "save status: ok after a good save");
    cfg_set("TOTK_VOLUME", "20");
    char bad[1100];
    snprintf(bad, sizeof bad, "%s", path);
    strcpy(strrchr(bad, '\\') + 1, "no-such-dir\\cfg-probe.ini");
    CHECK(cfg_save_to(bad) == 0 && cfg_last_save_ok() == 0 && EQ(cfg_get("TOTK_VOLUME"), "20"),
          "save status: an unwritable path fails, the value stays");
    cfg_set("TOTK_VOLUME", "21");
#ifdef _WIN32
    CHECK(plat_file_write(path, rbase, sizeof rbase - 1), "write the read-only ini");
    SetFileAttributesA(path, FILE_ATTRIBUTE_READONLY);
    CHECK(cfg_save_to(path) == 0 && cfg_last_save_ok() == 0 && EQ(cfg_get("TOTK_VOLUME"), "21"),
          "save status: a read-only file fails, cfg_get returns the new value");
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
#endif
    CHECK(cfg_save_to(path) == 1 && cfg_last_save_ok() == 1, "save status: the retry after the failure works");
    got = plat_file_read(path, &got_n);
    CHECK(got && strstr((const char *)got, "\nvolume = 21\n"), "save status: the retried save wrote the value");
    free(got);
    plat_file_delete(path);
    cfg_load(path);
    CHECK(rumble_motor(0, 0.6f, 1.3f) == 0 && rumble_motor(0.5f, 0.6f, 0) == 0, "rumble: zero amplitude or strength is silent");
    CHECK(rumble_motor(1, 0.6f, 3) == 65535 && rumble_motor(5, 1, 1) == 65535, "rumble: saturates at 65535");
    CHECK(rumble_motor(0.25f, 1, 1) > 16000 && rumble_motor(0.25f, 1, 1) < 16800, "rumble: curve 1, strength 100% is linear");
    CHECK(rumble_motor(0.1f, 0.6f, 1.3f) > rumble_motor(0.1f, 1, 1.3f), "rumble: curve lifts weak values");
    CHECK(rumble_motor(0.3f, 0.6f, 1.3f) > rumble_motor(0.3f, 0.6f, 1), "rumble: strength scales");
    // Keyboard and mouse bindings (input_map.c)
    uint8_t dn[KEY_COUNT] = {0};
    int32_t st[2];
    CHECK(input_map_buttons(dn) == 0, "keys: nothing held, no buttons");
    dn[KEY_MOUSE_L] = 1;
    CHECK(input_map_buttons(dn) == 1ull << B_Y, "keys: left mouse button is Y");
    dn[KEY_MOUSE_R] = dn[KEY_Z] = 1;
    CHECK(input_map_buttons(dn) == (1ull << B_Y | 1ull << B_ZL), "keys: right mouse button and Z both press ZL");
    memset(dn, 0, sizeof dn);
    dn[KEY_X] = dn[KEY_TAB] = dn[KEY_M] = dn[KEY_LCTRL] = dn[KEY_3] = 1;
    CHECK(input_map_buttons(dn) == (1ull << B_ZR | 1ull << B_PLUS | 1ull << B_MINUS | 1ull << B_STICK_L | 1ull << B_RIGHT),
          "keys: X ZR, Tab Plus, M Minus, Left Ctrl stick click, 3 D-pad right");
    memset(dn, 0, sizeof dn);
    dn[KEY_W] = 1;
    input_map_stick(dn, 0, st);
    CHECK(st[0] == 0 && st[1] == 32767, "keys: W is left stick up");
    dn[KEY_A] = 1;
    input_map_stick(dn, 0, st);
    CHECK(st[0] == -23170 && st[1] == 23170, "keys: W A is a diagonal on the unit circle");
    dn[KEY_W] = dn[KEY_A] = dn[KEY_S] = 1;
    dn[KEY_D] = 1;
    input_map_stick(dn, 0, st);
    CHECK(st[0] == 0 && st[1] == 0, "keys: opposite keys cancel");
    memset(dn, 0, sizeof dn);
    dn[KEY_LEFT] = 1;
    input_map_stick(dn, 1, st);
    CHECK(st[0] == -32767 && st[1] == 0, "keys: left arrow is right stick left");
    input_map_stick(dn, 0, st);
    CHECK(st[0] == 0 && st[1] == 0, "keys: arrows leave the left stick alone");
    input_mouse_stick(30, 0, 1.0 / 60, 1, st);
    CHECK(st[0] == 32767 * 1800 / 3000 && st[1] == 0, "mouse: 30 counts in 1/60 s is 60% right");
    input_mouse_stick(0, -10, 1.0 / 60, 1, st);
    CHECK(st[0] == 0 && st[1] > 0, "mouse: moving up is stick up");
    input_mouse_stick(30, 0, 1.0 / 30, 1, st);
    CHECK(st[0] == 32767 * 900 / 3000, "mouse: the same counts over twice the time are half the deflection");
    input_mouse_stick(15, 0, 1.0 / 60, 2, st);
    CHECK(st[0] == 32767 * 1800 / 3000, "mouse: doubling sensitivity halves the counts needed (15 counts at 2 = 30 at 1)");
    input_mouse_stick(1000, 1000, 1.0 / 60, 1, st);
    CHECK((int64_t)st[0] * st[0] + (int64_t)st[1] * st[1] <= 32768ll * 32768, "mouse: clamped to the unit circle");
    input_mouse_stick(30, 0, 1.0 / 60, 0, st);
    CHECK(st[0] == 0 && st[1] == 0, "mouse: sensitivity 0 is off");
    printf("probe-cfg: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
