// totk.ini: file-backed defaults for the TOTK_* environment variables (docs/controls.md, totk.ini.example).
// Format: "key = value" lines, '#' or ';' at the start of a line begins a comment, [sections] only group. A key is a
// variable name without "TOTK_" (an optional TOTK_ prefix is accepted), case-insensitive. A value is the rest of the
// line, trimmed, optionally in double quotes; an empty value counts as unset. The environment wins over the file.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

#define PREFIX "TOTK_"
#define PREFIX_LEN 5
#define CFG_MAX_KEYS 512

typedef struct { char *key, *value; } CfgEntry;   // key upper-case, without the prefix

static CfgEntry entries[CFG_MAX_KEYS];
static int n_entries;

// Every name the runtime reads (and the patch sets in generated/), without the prefix. A file key outside this list is
// logged as unknown. A new variable goes here too, or its ini key logs a harmless "unknown key" line.
static const char *const known[] = {
    "APP_POOL", "ASSUME_FOCUS", "AUDIO", "AUDIO_OFF", "AUDIO_PREFILL_MS", "BACKGROUND_WINDOW", "BIND_STATS",
    "CLOTH_REALTIME", "DUMP", "DUMP_BUF", "DUMP_DIR", "DUMP_LOG", "EXEFS", "FEEDBACK", "FIX_C0", "FIX_ZERO", "FLIGHT",
    "FLIGHT_AT", "FLIGHT_DIR", "FLIGHT_FRAMES", "FLIGHT_SPIKE_MS", "FLIGHT_THUMBS", "FPS60", "FREE_CORES", "GPL",
    "GPL_DRAW_BUILD",
    "GPU", "GPU_INDEX", "GPU_NAME", "HIGHFPS_FIX", "INPUT", "INPUT_RECORD", "INPUT_SCRIPT", "LANG", "LIB_PREBUILD",
    "LIB_PREBUILD_KEEP", "LIB_PREBUILD_THREADS", "LOCALE", "LOD_GUARD", "MINIMAL_LAYOUT", "MOTION", "MOVIE_HOST",
    "MOVIE_PACE", "OPERATION_MODE", "PERF_SLOW_MS", "PIPELINE_CACHE", "PIPE_PREBUILD", "PIPE_SYNC", "PREWARM_XLATE",
    "PROBE_MOVIE", "PROBE_MOVIE_SHOT", "PROFILE", "PROFILE_DIR", "PROFILE_FRAMES", "PROFILE_RENDER", "PUSH_STAGE", "RDOC",
    "RDOC_DIR", "RDOC_DLL", "REALTIME", "REEXEC", "ROMFS", "ROMFS_INDEX", "ROMFS_MMAP", "ROMFS_MODS", "ROMFS_PATCHES",
    "ROMFS_PROOF_DIR", "ROMFS_VERIFY", "RUMBLE", "SHADER_CACHE", "SHARED_FONTS", "SHOTS", "SHOTS_DIR", "SHOTS_FULL",
    "SPEED_PROBE", "SPLIT_DRAWS", "SWKBD_ANSWER", "SWKBD_TEST", "TEST_PREPARE_SHADERS", "TEX_ALIAS", "TIMER_1MS",
    "TRACE", "VK_NO_FLOAT64", "VK_PRESENT", "VK_SSBO_ALIGN", "VK_SUBGROUP", "VK_SUBGROUP_PIN", "VK_VALIDATION",
    "VS_OUTS", "XLATE", "XLATE_ID", "XLATE_PROCS",
};

static int is_known(const char *key) {
    for (size_t i = 0; i < sizeof known / sizeof *known; i++)
        if (!strcmp(known[i], key)) return 1;
    return 0;
}

static void clear(void) {
    for (int i = 0; i < n_entries; i++) free(entries[i].key), free(entries[i].value);
    n_entries = 0;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

static void add(char *key, char *value, int line) {
    for (char *p = key; *p; p++) *p = (char)toupper((unsigned char)*p);
    if (!strncmp(key, PREFIX, PREFIX_LEN)) key += PREFIX_LEN;
    size_t vl = strlen(value);
    if (vl >= 2 && value[0] == '"' && value[vl - 1] == '"') value[vl - 1] = 0, value++;
    if (!*key) return;
    if (!is_known(key)) rt_log("cfg: totk.ini line %d: unknown key '%s'", line, key);
    if (!*value) return;   // "key =" leaves the setting at its default
    for (int i = 0; i < n_entries; i++)
        if (!strcmp(entries[i].key, key)) {   // a repeated key: the later line wins
            free(entries[i].value);
            entries[i].value = _strdup(value);
            return;
        }
    if (n_entries == CFG_MAX_KEYS) return;
    entries[n_entries++] = (CfgEntry){_strdup(key), _strdup(value)};
}

int cfg_load(const char *path) {
    clear();
    uint64_t size = 0;
    uint8_t *data = plat_file_read(path, &size);
    if (!data) return 0;
    char *p = (char *)data;
    if (size >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) p += 3;   // editors add a BOM
    for (int line = 1; *p; line++) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = 0;
        char *s = trim(p);
        char *eq = strchr(s, '=');
        if (*s && *s != '#' && *s != ';' && *s != '[') {
            if (!eq) rt_log("cfg: totk.ini line %d: no '=' in '%s'", line, s);
            else {
                *eq = 0;
                add(trim(s), trim(eq + 1), line);
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    free(data);
    return 1;
}

const char *cfg_get(const char *name) {
    const char *e = getenv(name);
    if (e || strncmp(name, PREFIX, PREFIX_LEN)) return e;
    for (int i = 0; i < n_entries; i++)
        if (!_stricmp(entries[i].key, name + PREFIX_LEN)) return entries[i].value;
    return NULL;
}

void cfg_init(void) {
    char path[1024];
    if (!plat_exe_path(path, sizeof path)) return;
    char *cut = strrchr(path, '\\');
    if (!cut) cut = strrchr(path, '/');
    if (!cut || cut + 10 >= path + sizeof path) return;
    strcpy(cut + 1, "totk.ini");
    if (!cfg_load(path)) {
        rt_log("cfg: no settings file at %s", path);
        return;
    }
    rt_log("cfg: read %s: %d keys", path, n_entries);
    for (int i = 0; i < n_entries; i++) {
        char var[128];
        snprintf(var, sizeof var, PREFIX "%s", entries[i].key);
        if (getenv(var)) rt_log("cfg: %s is set in the environment and overrides totk.ini", var);
    }
}
