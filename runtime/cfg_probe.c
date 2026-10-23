// --probe-cfg: totk.ini parsing, the environment override and a missing file (cfg.c). Needs no game files.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"

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
    CHECK(rumble_motor(0, 0.6f, 1.3f) == 0 && rumble_motor(0.5f, 0.6f, 0) == 0, "rumble: zero amplitude or strength is silent");
    CHECK(rumble_motor(1, 0.6f, 3) == 65535 && rumble_motor(5, 1, 1) == 65535, "rumble: saturates at 65535");
    CHECK(rumble_motor(0.25f, 1, 1) > 16000 && rumble_motor(0.25f, 1, 1) < 16800, "rumble: curve 1, strength 100% is linear");
    CHECK(rumble_motor(0.1f, 0.6f, 1.3f) > rumble_motor(0.1f, 1, 1.3f), "rumble: curve lifts weak values");
    CHECK(rumble_motor(0.3f, 0.6f, 1.3f) > rumble_motor(0.3f, 0.6f, 1), "rumble: strength scales");
    printf("probe-cfg: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
