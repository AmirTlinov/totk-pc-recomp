# Windows host behaviour

What the Windows host does differently from the console and from MacNativeGames' macOS host, measured on machine A
(Ryzen 9 7900X, RX 9070, NVMe) in 2026-10. Each entry: the behaviour, the numbers, where the runtime handles it, and
the rule for new code. Linux equivalents of the fixes: [backlog.md](../backlog.md) item 35.

## Rules

- The game threads (guest main, job workers) and the render thread never open, read, write or flush a file, and
  never wait for a thread that does. Move file I/O to a background thread and hand results over in memory.
- Short waits spin; waits over 1 ms use the high-resolution timer.
- Every frame-time claim needs three runs per build: single runs of one recording differ by up to three times.

## Timers and time

- **Waitable timer overshoot.** A high-resolution waitable timer ends a wait 0.3-0.5 ms late at best: the SDK's
  20 us polling sleeps (main thread and job workers, about 1.5 per frame each) took 460-515 us. `kernel.c`
  `sleep_ns` spins on the performance counter up to `SLEEP_SPIN_NS` (1 ms), the virtual core already given up.
  macOS `nanosleep` is accurate, so MacNativeGames never had this. Measure with `TOTK_TRACE=sched` ("sleep asked /
  took" per thread).
- **A 1 ms timer thread does not help.** A run with a thread waking every 1 ms (the sampler of `TOTK_PROFILE_MAIN`)
  had more slow frames than one without, not fewer: no evidence for core parking or C-state wake-up costs here.
- **Counter conversion.** `QueryPerformanceCounter` ticks to ns or to the guest's 19.2 MHz counter as
  `(unsigned __int128)t * num / den` compiles to `__udivti3`, a library call; it took a tenth of the guest threads'
  running time in a fight, as the scheduler and the guest's counter reads convert many thousands of times a second.
  `platform.h` `PlatScale` multiplies by a precomputed integer and 64-bit fraction (branch `task/perf2`). A lazily
  set two-field scale shared by threads must be published behind a flag stored after it.

## Files

- **Small-file opens stall behind other writers.** Opening a 20-byte file in a 139k-file cache folder takes 0.1 ms
  alone (p99 0.3 ms) but took 11-40 ms in play, while the translator, the pipeline cache and the save wrote and
  flushed on the same volume. Defender's real-time scan was off; the stall is the file system. `gfx_shader.c` reads
  all `.use` masks on a background thread at start (45k files in 12 s).
- **Flushes are expensive.** A save Commit (copy or hard-link 82 files, `FlushFileBuffers` on each file and directory,
  two `MoveFileExW` with `MOVEFILE_WRITE_THROUGH`, rotate backups) took 93-135 ms: compare 23-41, hard links 17-31,
  copy 0-18, directory flush 22-24, oldest backup deletion 16. `fs_host.c` runs it on a thread; game writes wait for it.
- **Saving a cache holds its lock.** `vkGetPipelineCacheData` plus the file write took about 60 ms; done on the present
  thread under the pipeline lock, every 20 s autosave froze the replay. `vk_pipeline.c` saves on its own thread
  under a separate lock.
- **A crash leaves zero-filled files.** After bugchecks, thousands of `.spv` and `.res.json` files were zero-filled
  (3647 of 21924 once). Validate cached files on load (`gfx_shader.c` checks the `.res.json` shape) instead of
  trusting their presence.
- **Unicode paths.** Use the W APIs with UTF-8 to UTF-16 conversion (`fs_wide`); exes that take paths carry an
  `activeCodePage` UTF-8 manifest ([installer.md](../installer.md), Constraints).

## Threads and sampling

- **Stopping a thread waits for user mode.** `SuspendThread` returns at once, but `GetThreadContext` waits until the
  thread has left the kernel; a thread inside a long kernel call (a file open, a driver call) stalls the sampler for
  that long. `prof_win.c` records such stops over 2 ms as `tstall`: they name the thread's kernel call.
- **Guard page.** A thread stopped between its `sub rsp` and the stack probe has Rsp inside its guard page; copying
  the stack from Rsp raised `STATUS_GUARD_PAGE_VIOLATION` and ended the process. Check `PAGE_GUARD` with
  `VirtualQuery` before reading another thread's stack.
- **Idle threads.** `QueryThreadCycleTime` tells without suspending whether a thread ran since the last look; the
  sampler repeats an idle thread's last chain instead of stopping it.
- **Three virtual cores are the CPU budget.** TotK keys per-core data by `GetCurrentProcessorNumber` without locks,
  so guest threads share three virtual cores whatever the host has. In a fight the guest main thread and five job
  workers keep 2.6 of 3 busy and ready threads queue for a core: guest code speed decides the frame rate there.

## Vulkan and the driver (AMD, Windows)

- **New memory blocks.** VMA's `vkAllocateMemory` of a 256 MiB device-local block took 8-11 ms (memory type 3:
  11-19 ms, 650 MiB: 23 ms) under VMA's block lock; texture creation on the render thread paid it four times in the
  first minute of play while VRAM grew from 3.0 to 4.0 GB. Each allocation over 2 ms is logged by `vk_device.c`
  `timed_allocate` (branch `task/perf2`).
- **Driver pipeline cache.** Keyed by the exe path and limited in size; preparing another build dir can evict this
  one's libraries ([backlog.md](../backlog.md) item 12, [agent-playbook.md](../agent-playbook.md)).
- `win32u.dll` frames in a host stack are kernel graphics calls (WDDM), usually memory allocation or submission.

## Measuring

- **Run-to-run spread.** One recording replayed on near-identical builds gave 1% lows from 31.5 to 50 fps; three runs
  of one build agreed within 46-50. Compare medians of three `--quiet` runs, never one run against another.
- **Replays need game time.** With the real-time frame step, input keyed by presents lands at other game times at
  another frame rate; recordings key input by the world clock (`frame_rate.c`), and a load of any length no longer
  shifts it. The fight still diverges after about 15 game seconds (enemy AI, physics).
- **Bit flips on machine A.** Its RAM flips bits (bugcheck 0x139 and others, several a day): a cached file, a
  hard-linked object or a release can carry a flip without an error. Compare two independent builds; objects embed
  the toolchain path of the folder they were built in, so compare their `.text`, `.data` and `.rdata`, not whole
  files. A file that reads wrong only through the file cache: hash it with `FILE_FLAG_NO_BUFFERING` too.
- **Swapping exes.** `build_game.py` does not relink when only `totk.exe` was replaced by a copy; delete the exe
  before rebuilding to restore a build dir.
