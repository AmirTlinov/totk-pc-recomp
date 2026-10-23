// Windows implementation of platform.h: CreateThread, QueryPerformanceCounter, high-resolution waitable timers,
// SRW locks and condition variables.
#include <stdio.h>
#include <windows.h>
#include <stdlib.h>
#include <wchar.h>

#include "platform.h"

_Static_assert(sizeof(PlatMutex) == sizeof(SRWLOCK), "PlatMutex holds an SRWLOCK");
_Static_assert(sizeof(PlatCond) == sizeof(CONDITION_VARIABLE), "PlatCond holds a CONDITION_VARIABLE");

// ---- threads

struct PlatThread {
    HANDLE h;
    PlatThreadFn fn;
    void *arg;
};

static DWORD WINAPI thread_main(LPVOID p) {
    PlatThread *t = p;
    t->fn(t->arg);
    return 0;
}

PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    t->h = CreateThread(NULL, 0, thread_main, t, CREATE_SUSPENDED, NULL);
    if (!t->h) { free(t); return NULL; }
    if (name) {
        wchar_t w[64];
        if (MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64)) SetThreadDescription(t->h, w);
    }
    ResumeThread(t->h);
    return t;
}

void plat_thread_join(PlatThread *t) {
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

// The thread keeps using t->fn and t->arg, so the struct stays allocated (a few bytes per detached thread).
void plat_thread_detach(PlatThread *t) {
    CloseHandle(t->h);
    t->h = NULL;
}

uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask) {
    return (uint64_t)SetThreadAffinityMask(t ? t->h : GetCurrentThread(), (DWORD_PTR)mask);
}

uint32_t plat_cpu_count(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? (uint32_t)n : 1;
}

// ---- time

static uint64_t qpc_freq(void) {
    static uint64_t freq;
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (uint64_t)f.QuadPart;
    }
    return freq;
}

uint64_t plat_now_ns(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((unsigned __int128)(uint64_t)t.QuadPart * 1000000000u / qpc_freq());
}

// A high-resolution waitable timer per sleeping thread (Windows 10 1803+; one handle per thread, kept until exit)
// wakes within about 0.1-0.5 ms; the last PLAT_SPIN_NS are spent yielding so the deadline is met closely.
// Without that timer kind, Sleep(1) granularity applies.
#define PLAT_SPIN_NS 300000u

static __thread HANDLE sleep_timer;
static __thread int sleep_timer_tried;

void plat_sleep_ns(uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    if (!sleep_timer_tried) {
        sleep_timer_tried = 1;
        sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    if (ns > PLAT_SPIN_NS) {
        uint64_t coarse = ns - PLAT_SPIN_NS;
        if (sleep_timer) {
            LARGE_INTEGER due = {.QuadPart = -(LONGLONG)(coarse / 100)};   // relative, 100 ns units
            if (SetWaitableTimerEx(sleep_timer, &due, 0, NULL, NULL, NULL, 0))
                WaitForSingleObject(sleep_timer, INFINITE);
            else Sleep((DWORD)(coarse / 1000000));
        } else Sleep((DWORD)(coarse / 1000000));
    }
    while (plat_now_ns() < end) SwitchToThread();
}

// ---- locks and condition variables

void plat_mutex_init(PlatMutex *m) { InitializeSRWLock((PSRWLOCK)m); }
void plat_mutex_lock(PlatMutex *m) { AcquireSRWLockExclusive((PSRWLOCK)m); }
int plat_mutex_trylock(PlatMutex *m) { return TryAcquireSRWLockExclusive((PSRWLOCK)m) != 0; }
void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)m); }

void plat_cond_init(PlatCond *c) { InitializeConditionVariable((PCONDITION_VARIABLE)c); }
void plat_cond_wait(PlatCond *c, PlatMutex *m) { SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, INFINITE, 0); }

int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    uint64_t ms = (ns + 999999) / 1000000;
    DWORD to = ms >= INFINITE ? INFINITE - 1 : (DWORD)ms;
    return SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, to, 0) ? 1 : GetLastError() != ERROR_TIMEOUT;
}

void plat_cond_signal(PlatCond *c) { WakeConditionVariable((PCONDITION_VARIABLE)c); }
void plat_cond_broadcast(PlatCond *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)c); }

// ---- files

static int widen(const char *s, wchar_t *w, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap);
    if (!n) w[0] = 0;
    return n;
}

#define WPATH 2048

int plat_file_exists(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void plat_file_mkdir_p(const char *path) {
    wchar_t tmp[WPATH];
    if (!widen(path, tmp, WPATH)) return;
    for (wchar_t *p = tmp + 1; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            wchar_t c = *p;
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = c;
        }
    }
    CreateDirectoryW(tmp, NULL);
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    uint8_t *buf = NULL;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1ll << 30)) {
        buf = malloc((size_t)sz.QuadPart + 1);
        DWORD got = 0;
        if (buf && (!ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) || got != sz.QuadPart)) {
            free(buf);
            buf = NULL;
        }
        if (buf) {
            buf[sz.QuadPart] = 0;
            *size = (uint64_t)sz.QuadPart;
        }
    }
    CloseHandle(h);
    return buf;
}

int plat_file_write(const char *path, const void *data, size_t n) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    int ok = WriteFile(h, data, (DWORD)n, &put, NULL) && put == n;
    CloseHandle(h);
    return ok;
}

int plat_file_replace(const char *from, const char *to) {
    wchar_t a[WPATH], b[WPATH];
    if (!widen(from, a, WPATH) || !widen(to, b, WPATH)) return 0;
    return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    char tmp[WPATH + 48];
    wchar_t a[WPATH], b[WPATH];
    int len = snprintf(tmp, sizeof tmp, "%s.%lu.%lu.tmp", path, GetCurrentProcessId(), GetCurrentThreadId());
    if (len < 0 || len >= (int)sizeof tmp || !widen(tmp, a, WPATH) || !widen(path, b, WPATH)) return 0;
    HANDLE h = CreateFileW(a, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    int ok = WriteFile(h, data, (DWORD)n, &put, NULL) && put == n && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
    DeleteFileW(a);
    return 0;
}

int plat_file_flush(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    return ok;
}

void plat_file_delete(const char *path) {
    wchar_t w[WPATH];
    if (widen(path, w, WPATH)) DeleteFileW(w);
}

int plat_file_truncate(const char *path, uint64_t size) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER at = {.QuadPart = (LONGLONG)size};
    int ok = SetFilePointerEx(h, at, NULL, FILE_BEGIN) && SetEndOfFile(h);
    CloseHandle(h);
    return ok;
}

int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx) {
    wchar_t wd[WPATH], we[64], pat[WPATH + 64];
    if (!widen(dir, wd, WPATH) || !widen(ext, we, 64)) return 0;
    swprintf(pat, WPATH + 64, L"%ls\\*%ls", wd, we);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char name[1024];
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
        if (!cb(name, ctx)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 1;
}

int plat_mem_commit(uint64_t addr, size_t n) {
    return VirtualAlloc((void *)(uintptr_t)addr, n, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

int plat_mem_read(uint64_t addr, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)addr, out, n, &got) && got == n;
}

int plat_exe_path(char *out, size_t cap) {
    wchar_t w[WPATH];
    DWORD n = GetModuleFileNameW(NULL, w, WPATH);
    if (!n || n >= WPATH) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) != 0;
}

size_t plat_getenv(const char *name, char *out, size_t cap) {
    wchar_t wn[256], v[WPATH];
    if (!widen(name, wn, 256)) return 0;
    DWORD n = GetEnvironmentVariableW(wn, v, WPATH);
    if (!n || n >= WPATH) return 0;
    int m = WideCharToMultiByte(CP_UTF8, 0, v, -1, out, (int)cap, NULL, NULL);
    return m ? (size_t)m - 1 : 0;
}

uint32_t plat_thread_id(void) { return GetCurrentThreadId(); }

// ---- child processes

struct PlatChild {
    HANDLE proc, in, out;                                  // in: our write end, out: our read end
};

PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err) {
    wchar_t we[WPATH], wa[256] = L"", cmd[WPATH + 300];
    if (err) *err = 0;
    if (!widen(exe, we, WPATH) || (arg && !widen(arg, wa, 256))) return NULL;
    swprintf(cmd, WPATH + 300, arg ? L"\"%ls\" %ls" : L"\"%ls\"", we, wa);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE in_r, in_w, out_r, out_w;
    // Pipes are created and handed out one child at a time: a child started meanwhile would inherit the other pipe
    // ends and keep them open after their owner closed them.
    static SRWLOCK spawn = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&spawn);
    if (!CreatePipe(&in_r, &in_w, &sa, 0)) return ReleaseSRWLockExclusive(&spawn), NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        CloseHandle(in_r), CloseHandle(in_w);
        return ReleaseSRWLockExclusive(&spawn), NULL;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = in_r, .hStdOutput = out_w,
                       .hStdError = nul != INVALID_HANDLE_VALUE ? nul : out_w};
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    DWORD e = GetLastError();
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    ReleaseSRWLockExclusive(&spawn);
    if (!ok) {
        CloseHandle(in_w), CloseHandle(out_r);
        if (err) *err = e;
        return NULL;
    }
    CloseHandle(pi.hThread);
    PlatChild *c = malloc(sizeof *c);
    if (!c) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess), CloseHandle(in_w), CloseHandle(out_r);
        return NULL;
    }
    c->proc = pi.hProcess, c->in = in_w, c->out = out_r;
    return c;
}

void plat_child_stop(PlatChild *c) {
    if (!c) return;
    CloseHandle(c->in);
    CloseHandle(c->out);
    TerminateProcess(c->proc, 1);
    CloseHandle(c->proc);
    free(c);
}

int plat_child_write(PlatChild *c, const void *data, size_t n) {
    DWORD put = 0;
    return WriteFile(c->in, data, (DWORD)n, &put, NULL) && put == n;
}

long plat_child_read(PlatChild *c, void *buf, size_t cap) {
    DWORD got = 0;
    if (!ReadFile(c->out, buf, (DWORD)cap, &got, NULL)) return -1;
    return (long)got;
}
