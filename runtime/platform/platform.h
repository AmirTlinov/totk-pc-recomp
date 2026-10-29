// Platform layer: threads, time, locks. New runtime code calls this instead of the OS;
// one implementation per OS (win32.c). The FP control register lives in the per-architecture fp_*.h headers,
// inline, because lifted FP code touches it on every operation.
#pragma once
#include <stdint.h>

#if defined(__x86_64__) || defined(_M_X64)
#include "fp_x86.h"
#endif

// ---- threads
typedef struct PlatThread PlatThread;
typedef void (*PlatThreadFn)(void *arg);

// Starts fn(arg) on a new thread named name (shown in debuggers and profilers; may be NULL). NULL on failure.
PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name);
void plat_thread_join(PlatThread *t);     // waits for fn to return, then frees t
void plat_thread_detach(PlatThread *t);   // frees t; the thread runs on
// Restricts t (NULL: the calling thread) to the logical processors in mask (bit n = processor n of the first group).
// Returns the previous mask, 0 on failure.
uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask);
uint32_t plat_cpu_count(void);            // logical processors

// ---- time
uint64_t plat_now_ns(void);               // monotonic, high resolution, arbitrary epoch
// t * num / den for a fixed num / den without a 128-bit division per call (__udivti3 took 5% of the game's running
// samples through plat_now_ns and rt_ticks): t * q + (t * f >> 64) with q = num / den and f = (num % den) * 2^64 / den.
// Monotonic in t and at most one below the exact quotient while t * (num % den) / den fits 64 bits.
typedef struct { uint64_t q, f; } PlatScale;
static inline PlatScale plat_scale(uint64_t num, uint64_t den) {
    return (PlatScale){num / den, (uint64_t)(((unsigned __int128)(num % den) << 64) / den)};
}
static inline uint64_t plat_scaled(PlatScale s, uint64_t t) {
    return t * s.q + (uint64_t)(((unsigned __int128)t * s.f) >> 64);
}
void plat_sleep_ns(uint64_t ns);          // returns no earlier than ns later; overshoot well under 1 ms

// ---- locks (not recursive) and condition variables; zero-initialised storage is a valid unlocked lock and an empty
// condition variable (PLAT_MUTEX_INIT / PLAT_COND_INIT spell it).
typedef struct { void *p; } PlatMutex;
typedef struct { void *p; } PlatCond;
#define PLAT_MUTEX_INIT {0}
#define PLAT_COND_INIT {0}
void plat_mutex_init(PlatMutex *m);
void plat_mutex_lock(PlatMutex *m);
int plat_mutex_trylock(PlatMutex *m);     // 1 when acquired
void plat_mutex_unlock(PlatMutex *m);
void plat_cond_init(PlatCond *c);
void plat_cond_wait(PlatCond *c, PlatMutex *m);
int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns);   // 0 on timeout (spurious wakeups return 1 too)
void plat_cond_signal(PlatCond *c);
void plat_cond_broadcast(PlatCond *c);

// ---- memory
int plat_mem_commit(uint64_t addr, size_t n);             // read-write pages inside a reserved range; 1 on success
int plat_mem_read(uint64_t addr, void *out, size_t n);    // copies n bytes, 0 instead of faulting on unmapped memory

// ---- files. Paths are UTF-8 with either separator; each implementation converts to what the OS wants.
int plat_file_exists(const char *path);                   // 1 for a file (not a directory)
void plat_file_mkdir_p(const char *path);                 // creates every missing directory of path, itself included
// Whole file into a malloc'd buffer with a NUL after the last byte; NULL when missing, empty or over 1 GiB.
uint8_t *plat_file_read(const char *path, uint64_t *size);
int plat_file_write(const char *path, const void *data, size_t n);   // create or truncate; 1 when all n bytes landed
int plat_file_replace(const char *from, const char *to);  // rename, replacing to (write-through); 1 on success
// Writes path.<pid>.<tid>.tmp, flushes it to the disk and renames it over path: a crash leaves the old file or the new
// one, never a torn or zero-filled one. 1 on success.
int plat_file_write_atomic(const char *path, const void *data, size_t n);
int plat_file_flush(const char *path);                   // flushes a written file's data to the disk; 1 on success
void plat_file_delete(const char *path);
int plat_file_truncate(const char *path, uint64_t size);  // 1 on success
int64_t plat_file_size(const char *path);                 // bytes of a file, -1 when missing or unreadable
int plat_file_set_readonly(const char *path, int on);     // sets or clears the read-only attribute; 1 on success
// A file that only grows and other processes may read meanwhile: each write lands whole at its end, from any thread,
// and reaches the file even if the process dies right after (no user-space buffer, no flush to the disk).
typedef struct PlatAppend PlatAppend;
PlatAppend *plat_append_open(const char *path);           // creates or truncates path; NULL on failure
int plat_append_write(PlatAppend *f, const void *data, size_t n);   // 1 when all n bytes landed
// Calls cb(name, ctx) for each file in dir whose name ends in ext (".in"); name has no directory part. cb returns 0 to
// stop. Returns 0 when dir cannot be listed.
int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx);
int plat_exe_path(char *out, size_t cap);                 // path of the running executable; 0 on failure
// Environment variable as UTF-8 into out; its length, or 0 when unset, empty or too long for cap.
size_t plat_getenv(const char *name, char *out, size_t cap);
// The user's UI language as a BCP-47 tag ("ru-RU", "en-US"); "" when the system gives none. Valid for the process.
const char *plat_ui_language(void);
uint32_t plat_thread_id(void);                            // id of the calling thread, for unique file names
// A system font file with the glyphs of a catalog language ("ja", "ko", "zh-Hans", "zh-Hant"): the menu shows each
// language's name in its own script. Copies the UTF-8 path into out; 0 when the system has none.
int plat_font_path(const char *lang, char *out, size_t cap);
// The operating system and its build ("Windows 10.0.28000") into out; 0 when unknown.
int plat_os_version(char *out, size_t cap);
// The file the process's stderr (the log) goes to, as a UTF-8 path; 0 when stderr is not a file (a console, a pipe).
int plat_stderr_path(char *out, size_t cap);

// ---- desktop: the in-game menu's "Copy system info" and "Open log folder"
int plat_clipboard_set(const char *utf8);                 // replaces the clipboard's text; 1 on success
int plat_open_folder(const char *path);                   // shows the folder in the file manager; 1 on success

// ---- dynamic libraries: a library some machines lack (a GPU vendor's), loaded at run time. A bare name is searched
// in the system directories only. NULL when missing.
void *plat_lib_open(const char *name);
void *plat_lib_sym(void *lib, const char *name);

// ---- machine load for the performance overlay (perfmon.c's sampling thread; each call can take milliseconds). A number
// the system does not give is -1.
typedef struct PlatLoad PlatLoad;
// Opens the counters: CPU times, the thermal zones, and the GPU engine and memory counters of the adapter whose LUID is
// gpu_luid (8 bytes, Vulkan's deviceLUID; NULL: the busiest adapter). Never NULL.
PlatLoad *plat_load_open(const uint8_t *gpu_luid);
void plat_load_close(PlatLoad *l);
// Since the previous call on l (the first call gives -1): CPU use of the machine and of this process, percent of all
// logical processors, and the hottest ACPI thermal zone in degrees Celsius.
void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c);
// Since the previous call on l: the busiest GPU engine type in percent (as Task Manager shows it), and the adapter's
// dedicated memory in use in MB. Any GPU vendor.
void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb);
// Physical memory in MB: in use and installed on the machine, and this process's working set.
void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb);

// ---- child processes: a program with stdin and stdout piped to us (stderr discarded), no console window.
typedef struct PlatChild PlatChild;
// Starts "exe" followed by arg (arg may be NULL). NULL on failure; *err (may be NULL) gets the OS error code.
// Children start one at a time so none inherits another pipe ends.
PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err);
void plat_child_stop(PlatChild *c);                       // kills the child and frees c
int plat_child_write(PlatChild *c, const void *data, size_t n);   // 1 when all n bytes were written
long plat_child_read(PlatChild *c, void *buf, size_t cap);        // bytes read (at least 1); 0 or negative: pipe broke
