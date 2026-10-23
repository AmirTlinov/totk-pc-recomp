// Settings: the TOTK_* environment variables, with totk.ini next to totk.exe behind them.
#pragma once

// getenv for a setting: the environment variable when set, else the totk.ini key (the name without "TOTK_", any
// case), else NULL. Same string semantics as getenv; the pointer stays valid for the process. A name without the
// TOTK_ prefix goes to getenv unchanged. Safe from any thread once cfg_init has returned.
const char *cfg_get(const char *name);

// Reads totk.ini once, first thing in main. Logs the file, its key count, unknown keys and environment overrides.
void cfg_init(void);

// Replaces the loaded keys with those of the file at path (tests). 1 when the file was read, 0 when missing.
// Not thread-safe: nothing else may call cfg_get meanwhile.
int cfg_load(const char *path);

