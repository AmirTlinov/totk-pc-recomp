// Platform layer: threads, time, locks. New runtime code calls this instead of the OS;
// one implementation per OS (win32.c in M5). The FP control register lives in the per-architecture fp_*.h headers,
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
// Calls cb(name, ctx) for each file in dir whose name ends in ext (".in"); name has no directory part. cb returns 0 to
// stop. Returns 0 when dir cannot be listed.
int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx);
int plat_exe_path(char *out, size_t cap);                 // path of the running executable; 0 on failure
// Environment variable as UTF-8 into out; its length, or 0 when unset, empty or too long for cap.
size_t plat_getenv(const char *name, char *out, size_t cap);
// The user's UI language as a BCP-47 tag ("ru-RU", "en-US"); "" when the system gives none. Valid for the process.
const char *plat_ui_language(void);
uint32_t plat_thread_id(void);                            // id of the calling thread, for unique file names

// ---- child processes: a program with stdin and stdout piped to us (stderr discarded), no console window.
typedef struct PlatChild PlatChild;
// Starts "exe" followed by arg (arg may be NULL). NULL on failure; *err (may be NULL) gets the OS error code.
// Children start one at a time so none inherits another pipe ends.
PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err);
void plat_child_stop(PlatChild *c);                       // kills the child and frees c
int plat_child_write(PlatChild *c, const void *data, size_t n);   // 1 when all n bytes were written
long plat_child_read(PlatChild *c, void *buf, size_t cap);        // bytes read (at least 1); 0 or negative: pipe broke
