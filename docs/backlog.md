# Backlog

The open work, in priority order: no artifacts and no crashes first, then 60 fps without dips, then other GPUs and
platforms. Each item states its evidence and the number or observation that decides it. Maxwell item numbers refer to
[research/maxwell.md](research/maxwell.md), lesson ids (S1, G3, ...) to [research/port-lessons.md](research/port-lessons.md).
Remove an item when it is merged or dropped.

Test machines: A, Ryzen 9 7900X + RX 9070 (development); B, laptop with a GTX 1050 Ti (4 GiB, no ReBAR) and an 8th- to
10th-generation Core i7; C, Core i7 of the same generations with Intel UHD 630. An RTX 3090 Ti was measured once. Only
machine A runs scripted routes; B and C send logs from the user's play.

## Bugs

1. **Yunobo vanishes.** He fades out of the main view about 45 frames after his ability is triggered (the ball fades
   over 15 frames, then is gone); his shadow-map draws stay. Ruled out: occlusion counts, CPU readback of GPU targets,
   missing NVN entry points, GPU-side resources. The game itself stops issuing his main-view draws. Leading hypothesis:
   game logic fades the model alpha (material c11[33].w) and drops it from the draw queues on a CPU-side input
   (camera-near fade, GfxColFade, CancelDraw distance or an AI fade). Next: a cheap check with the 60 fps and
   real-time patches off; then find his c11 buffer, watch writes to c11[33].w and walk up from the writer in Ghidra.
   Repro: the user's recorded session `yunobo-cast2` from save `user-save-2026-10-24-yunobo1`, 1360 frames.
2. **Black patches for a moment on machine B**, every time the Purah Pad opens (right stick click, keyboard C). Not
   the VRAM mirror: a verify run that caught the glitch compared 4.45 million mirror pages with 0 mismatches. Leading
   hypothesis: a new texture descriptor over a GPU-rendered texture fails `alias_compatible` and starts as a
   zero-cleared image in its own memory (`vk_texture.c` `entry_of`, `create_tex`, `init_image`). The run's two largest
   `left uninitialised` counts (33, 41) fall in its last seconds, where the glitch was. `cannot alias its pool memory`
   fills its 16-line log limit on NVIDIA and on the RX 9070 alike. Next: per-alias diagnostic lines and a
   `TOTK_TEX_FILL=magenta` knob, then a user run. Same class as yuzu PR 10418 ("areas black for a frame on camera
   changes", Maxwell 2.3 / check 7).
3. **Phantom objects and the flat far field**: for one frame a rock or log stands in place of pillars, or a surface
   fills the sky, most often while running or falling from a sky island. The game's producer rewrites its per-object
   uniform blocks 14-24 ms after the submit that reads them, with no fence; NVN starts the GPU within microseconds,
   our replay takes longer. A guard in `nvn_api_core.c` holds the producer until the newest submit has completed;
   whether it removes every case is waiting for play. Any replay or frame-overlap change must keep that guarantee.
   Scheduling experiments so far: port-lessons S1, S4.
4. **Lantern texture bug**: waiting for the user's description or a cropped screenshot.
5. **Fire on grass deals no damage and grass does not burn.** The CPU reads GPU-written data in GPU-only pools that
   are never written back (Maxwell 4.1). Recheck first: GPU textures are now written back on pool invalidates and
   fence waits (01889c7, which brought grass cutting back).
6. **Validation messages left** in world runs: `cp_t_tcb_20` samples an R32F view with depth compare in
   `vkCmdDispatch` (VUID-vkCmdDispatch-None-06479; needs a D32 copy of the 4x4 R32F placeholder, Maxwell check 20);
   "VkDescriptorSet 0x0 does not have a binding for vp_c1" in `vkCmdDrawIndexed`; a fragment write to Location 0
   with no colour attachment.
7. **To check in play**: F11, menus, movies, rumble and gyro; the game's actions on the keyboard layout and the order
   of the D-pad keys 1-4. Gyro: the user's logs show only Xbox pads, which have no gyro. With a DualSense or Pro
   Controller, check Steam Input (it presents pads as a virtual Xbox pad without sensors) and the game's own
   motion-aiming option.

## Frame time and freezes

8. **Replay on weaker CPUs.** With the VRAM mirror, machine B renders the GPU part in 10-11 ms, but the replay takes
   25-30 ms per frame (5.2 ms on machine A), so 25-45 fps. A `sched` run rules out core contention: the render thread
   spends 79-88% of the wall time on the CPU inside `nvnQueueSubmitCommands`, guest threads never preempt it and the
   OS takes about 5%. Mirror bookkeeping explains 2-3 ms. Suspects: NVIDIA driver cost per command, mirror and texture
   checks inside the replay. Since then the descriptor build skip, update templates and the shader memo cut the
   field replay on machine A from 8.87 to 7.99 ms. Next: a render-thread profile from machine B
   (`TOTK_PROFILE_RENDER=1`; the profile is written when the window closes).
9. **Replay thread** (`TOTK_REPLAY_ASYNC=1`, merged off by default). On machine A no fps gain: fights are bound by
   the game threads, field render busy time drops from 10 to 1.3 ms. Where the replay is longer than the frame
   (machine B) there is nothing to overlap: the game's main thread waits each frame for the previous frame's GPU
   work. Next: a user run on machine B with `replay_async = 1`.
10. **Texture uploads in gameplay**, now the largest cause of long frames. Step 1 splits the cost (image and memory
    creation, CPU deswizzle, staging, copies, locks) before choosing between a worker-thread upload, GPU deswizzle,
    image pooling or a per-frame budget. Dropped after measuring: narrower guest-written marks for `WriteTexels` and
    box-only image updates (re-uploads after `WriteTexels` are 0.003-0.05% of frame time).
11. **Asynchronous virtual-core preemption** (`TOTK_ASYNC_PREEMPT`, merged on by default; port-lessons S7). It cuts
    the waits it targets (`TOTK_CORE_DIAG`: game-class waits of 1 ms or more 53 / 41 / 27 to 4 / 10 / 18 in field /
    fight / a larger fight), but A/B timings show no mean-fps gain: fight frames over 25 ms 62 without, 96 with, over
    four pairs (58.7 / 58.2 fps, inside run-to-run noise). One of three field runs without it froze 7 s with the render
    thread blocked on nothing; no run with it did. Next: user play with it on, and `TOTK_ASYNC_PREEMPT=0` first on any
    new hang or stutter. If fights stay worse, try a shorter first spin (`AP_FIRST_NS`, 250 us) or mode 2.
12. **Driver cache re-warm.** The AMD driver keys its pipeline library cache by exe path, so a moved folder stutters
    until `totk.exe --prepare-shaders` runs again. Branch `task/rewarm` (not merged, never run in game) stamps
    `shader-prepared.bin` with device, driver and exe path and re-warms at start on a mismatch. Check whether NVIDIA
    and older AMD drivers cache the same way. Needed before machines B and C get an installer build.
13. **Shared shader list.** Newer MacNativeGames lists collapse variants that differ only in texture kind into `"*"`
    rows, which our runtime cannot match (it translates for the exact kind); `tools/prepare_shaders.py` reads `"*"` as
    2D. Option: match `"*"` rows at run time, as MacNativeGames' `prepare_shader_seed.py` does.
14. **Freeze census**: a 10-15 minute user session with `TOTK_FLIGHT=1`, `TOTK_TRACE=perf` and `TOTK_INPUT_RECORD`,
    ranking every frame over 50 ms by cause. Decides items 15-17.
15. `TOTK_PIPE_SKIP=fullscreen` as the default. On a cold larger fight: 0 frames over 100 ms, 1% low 33.9 to 41.7 fps.
    Risk: one frame without a full-screen pass the first time its pipeline appears.
16. **ASTC decode budget.** The largest batch, the 169 M texel 8x8 array at boot, scales to about 375 ms of GPU time on
    a GTX 1050 Ti. Split it by mip level with `minLod` if runs on machine B (`TOTK_ASTC_TIME=1`) show decodes in play.
17. **ASTC to BC7 re-encode on the GPU** for 4 GB cards: the same array is 677 MB as RGBA8 against 42 MB of blocks.
18. **Spin-wait gates** (MacNativeGames a038e6b, from nxnative 066f2af / 6d34984, MIT): loops polling memory hand the
    core to a queued thread after 50 us. Survey TotK 1.2.1 for such loops first (port-lessons S3).
19. **Over the GPU memory budget**, wait for running work and drain deferred releases before failing (MacNativeGames
    4de7b4e). Matters on 4 GB cards.
20. **GPU memory over long sessions** (port-lessons P3), never measured. MacNativeGames' TotK reached its 5000 MB GPU
    budget after 2 h 43 min (their issue #72) and grew from 4.5 to 6.25 GiB resident; their dead-serial hash sets
    reached about 1 GiB after 3 h. Our textures are destroyed when their pool range goes or a new texture takes the
    address (`vk_texture.c` `entry_of`, `vk_texture_range_gone`), pools at Finalize, Vulkan objects deferred.
    `mem: vram` (every 600 frames with `TOTK_TRACE=perf`, `vk_memory.c` `vram_log`) splits device-local memory. A 16,000
    frame uncapped fight on A (`build/v121-vram/NOTES.md`): pools flat (import 1160 MiB host, physical 111 MiB), mirrors
    440 MiB, live textures 1635 MiB, released textures up to 825 MiB (1686 entries); 3.25 GB in all and flat from frame
    9000, no leak. The released entries are the grower: TextureFinalize keeps the image (`vk_texture_release`) for the
    finalize-and-init-again cycle of transient textures and for finalized slots that still name the storage, and
    `sweep_released` drops all of them at once only past 4096 entries, a count, not bytes (about 2 GB at this mix).
    The GTX 1050 Ti of machine B (budget 3620 MiB) reaches 3300-3386 MiB within 1-2 min of play, and from then on up to
    394 pools stay imported (`mem: mirror` "stay imported: budget"), so the GPU reads them over PCIe.
    Next:
    - Count the frames from release to reuse of each released entry, in `mem: vram`, on the fight and the field: where
      transient textures end and the tail that can go begins.
    - Drop released entries oldest first, one by one, never all at once, and only under pressure (device-local use past
      about 85% of the budget, before a mirror is refused); unchanged on large cards. A fixed cap below the transient
      working set would recreate images every frame and lose GPU-rendered content (flicker, empty tile maps).
    - Check on B: mirrors refused for the budget and fps before and after; then a 2-3 h session with travel.
    - 2-4 GB cards: ASTC kept as RGBA8 is 4x its size; BC1/BC3 recompression (yuzu, `maxwell.md` 2.13) is the larger
      saving.
21. **Later**: recording on several threads; descriptor buffers or bindless (touches the translator); CPU/GPU frame
    overlap as LAS Recomp 13398126d (only after item 9, and it must keep the guard of item 3); an x86-64-v2 build for
    CPUs without AVX2/FMA (SavageWindRecomp ae42014b4); render passes ended by uniform updates and clear-only work
    (port-lessons P2); faster save loading; asynchronous texture write-back; menus, chests and cutscenes at 30 fps under
    the 60 fps patch, as SavageWindRecomp does (`runtime/hle/framepace.c`, [prior-art.md](research/prior-art.md)
    section E); a trimmed pipeline key (SavageWindRecomp).

## Correctness queue

Worked in this order.

22. Shader census for the open graphics lessons (port-lessons G3, G6), offline with no game run: how many TotK shaders
    fetch from buffer textures and how many clears use a partial mask.
23. Image aliasing over GPU-only pool memory (Maxwell 2.3 / check 7): count distinct texture keys per address per
    frame; the flight recorder counts new aliases.
24. `VK_KHR_index_type_uint8` (Maxwell check 9): drops the CPU widening of 130-170 byte-index draws per frame.
25. Texture gather rounding on AMD (Maxwell 2.10): yuzu's sub-pixel offset at `ImageGather` fixed BotW shadow edges.
26. Horizon scheduling fidelity (port-lessons S1, S2, S3, S5, S6): wake a better-priority thread on the signaller's core
    at SVC return; pin ideal-core -2 and main threads to core 0. Count each case with `TOTK_TRACE=svc,sched` first;
    fight fps must not drop.
27. Line width 4.5 (Maxwell check 14): dynamic `wideLines`.
28. `vk_memory.c` `defer()` (unverified): an object destroyed while frame F is recorded is stamped with F-1 and may be
    freed before F's commands that use it are submitted.
29. Masked clears (Maxwell 1.3 / check 3): a fix was written and parked; merge one if the census (item 22) finds a
    partial-mask clear.
30. GPU timestamps (port-lessons T1): find whether dynamic resolution reads the report difference before writing real
    GPU times per report.
31. MacNativeGames since our pin `b75be8c` (245 commits): kernel sync `dab728a` (S6), NVN texture sizes `3c482a7`,
    CPU reads of small render targets `8a29e0a` (item 5), border colour `29dd997`. Add the lessons, move the pin and fix
    the `refs/MacNativeGames` line citations in `docs/`.
32. Maxwell sources not read yet (maxwell.md section 8).

## Release and platforms

Each platform item gets a written design first, agreed with the maintainer.

33. **Release**: fix what stops play on the test machines, then publish the repository and a first installer release
    ([installer.md](installer.md)). Runs on more GPUs (NVIDIA, Vega, weaker CPUs, [tester-guide.md](tester-guide.md))
    go into one table: GPU, driver, CPU, game version, fps in field and fight, hitches, errors and crashes, with a log
    per machine.
34. **Platform layer**: no Win32 outside `runtime/platform/`. `tools/check_platform.py` counts 1479 uses in 41 files
    and fails on `astc_probe.c`, `gfx_abi_probe.c`, `input.h`, `motion_fusion.c`, `movie_probe.c`, `ov_movie.c`,
    `swkbd_win.c`, `vk_astc.c` and `vk_exec_draw.c`.
35. **Linux**: `runtime/platform/linux.c`, split as SavageWindRecomp splits its windows, linux and macos files. Write
    tracking (`host.h` `mem_written_pages`, `mem_freeze` / `mem_thaw` / `mem_frozen`, used by the VRAM mirror and the
    texture checks): freezing maps to `mprotect` plus a `SIGSEGV` handler; a kernel write (`read()` into guest memory)
    returns `EFAULT` instead of faulting, so the same thaw-before-read sites apply. Written pages: the `PAGEMAP_SCAN`
    ioctl with write-protect matching (kernel 6.7+, made for Wine's `GetWriteWatch`); on older kernels, write-protect
    the mirrored and texture-bound pages after each sync and mark them in the fault handler (Ryujinx), uploading pools
    the game rewrites every frame whole instead of tracking them. Soft-dirty (`/proc/self/clear_refs`) is process-wide
    and slow, a last resort. Under Proton the Windows build already works: Wine emulates both.
36. **Steam Deck**: RADV offers Vulkan 1.3 and `VK_EXT_external_memory_host`; 4 Zen 2 cores, so expect 30-40 fps.
37. **Launcher**: one exe for Windows and Linux, SDL3 and Dear ImGui (SavageWindRecomp `runtime/ui/launcher.mm`), with
    Play, Settings, Game files and Quit. Settings that need a restart go there, and it is the recovery path when a
    setting stops the boot. The installer already covers checking, extracting, building and preparing shaders.

## Later

38. **In-game menu**. F1, Esc or Minus+Plus opens it; the game gets no input while it is open. Tabs Screen, Game,
    Sound, Controls, Menu and Advanced show option rows over the `cfg` table (`runtime/cfg.c`), with the description of
    the selected row beside them, English and Russian text from `runtime/ui_text.c`. Every change goes to `totk.ini`,
    keeping the user's comments; a key set in the environment shows greyed out, a key read only at start says "after
    restart". Advanced holds every remaining key by group, open the log folder, copy system information
    (`runtime/sysinfo.c`) and reset of all settings behind a confirmation. `TOTK_PERF_OVERLAY` (F10 cycles): `fps` one
    line, `full` a panel of fixed width from `runtime/perfmon.c` (CPU, GPU and memory load through PDH, temperature,
    power and clock through NVML on NVIDIA only, frame times with a graph, replay, GPU time and the render thread's
    waits). Dear ImGui is drawn after the blit into the swapchain image (`runtime/ui/menu.cpp`). On machine A the
    swapchain gets fifo for the default mailbox in 30 of 31 logs; the Presentation row says so.

    Left to do:
    1. Not yet checked by hand: keyboard and gamepad navigation (the long Advanced page, the reset confirmation), Copy
       system info and Open log folder, Quit, input right after closing, `totk.ini` locked by another program.
    2. AMD GPU temperature, power and clock (ADLX); the overlay shows "-" for them on machine A.
    3. Later pages: button remapping (LAS Recomp `runtime/ui/controls_ui.mm`), AMD FSR 1 or bilinear scaling of the
       900p image, gyro sensitivity.

    Rendering above the game's 900p is a separate study (a game-side resolution patch, as Switch mods do, or scaling
    every render target ourselves); MacNativeGames has neither. The software keyboard could move onto
    SavageWindRecomp's `osk.mm`.

39. **Game version 1.4.3**. Builds, boots and plays: the user played it from their own saves on 2026-10-26 without
    problems; the field save runs at 60 fps with game speed 1.000, REV15 audio without underruns, 0 unimplemented NVN
    calls, 0 validation errors; every probe passes on 1.4.3 and on 1.2.1; the jump-table check reports 0 disagreements
    on both; the installer takes either update. Saves carry over both ways. `prepare_shaders.py` seeds every shader's
    default variant, as MacNativeGames' seed does, and reads the list with the V rows its history dropped
    (`tools/shader_list.py`; the current list alone has 134 rows of 1.4.3 shaders): 29,587 variants, among them all
    1,095 that the field and intro runs met in play. Open:
    1. MacNativeGames types vertex inputs and colour outputs by the bound format, so one variant serves any format;
       ours still differ by those types (347 of 19,468 variants met on 1.2.1). No frame-time gain, only fewer
       translations on first sight.
    2. The installer end-to-end test (`tests/installer_e2e.py`) runs only on 1.2.1.
    3. MacNativeGames' 1.4.3 output, render and fixed resolution patches (`game_patches_v143.c`) are not ported; 1.2.1
       has none of them either.
    4. The opening cutscene (Zelda under Hyrule Castle) shows hard light bands on 1.4.3 and on 1.2.1 alike; whether
       that is the game's look or ours is not checked.
40. **Community ExeFS patches** (`.ips`, `.pchtxt`, each for one game version): apply them through the generator's
    patch table (`recomp/patches.py`). RomFS mods already load (`romfs.c`, `TOTK_ROMFS_MODS`). Code-module mods
    (exlaunch `subsdk9`) need porting one by one.
41. **Frame rates above 60 and VRR.**
42. **Map tour**: teleport Link across the map by writing position, time of day and weather in guest memory, to fill
    the caches on a first run and to serve as an automatic test (crashes, validation, fps per region).
