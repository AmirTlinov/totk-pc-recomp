# Backlog: the plan and the proposed fixes

Priorities: no artifacts and no crashes first, then 60+ fps without dips, then other GPUs and platforms. Each item
names its evidence; Maxwell item numbers refer to [research/maxwell.md](research/maxwell.md). Remove an item when it is
merged or dropped.

## Open work (2026-10-25)

Everything still to do, in one list. Items below this section give older detail. Merged into main on 2026-10-25:
shader memo and push skip, page-indexed pool resolve cache, region-only Read/WriteTexels, GOB-wide swizzle, Intel
legacy vertex-input fallback, all-thread profile chains (04fb67b); descriptor build skip and update templates
(283bc70; field replay 8.87 to 7.99 ms); ASTC decode in a compute shader and native ASTC where sampled (0a0584e;
monsters2 worst frame 340 to 139 ms); waiting draws' stage libraries on idle workers and skipping draws into passes
with a per-frame target (3753939; cold monsters2 worst 257 to 131 ms). Dropped after measuring: narrower guest-written
marks for WriteTexels and box-only image updates (re-uploads after WriteTexels are 0.003-0.05% of frame time;
`build/v121-astcgpu/texels-narrow.patch`, `build/v121-texbox/texbox-wip.patch`).

Measure before building: every item states the number that decides it.

**Freezes and frame time**

1. Replay thread (5a.1): `task/replayq`, `build/v121-replayq/HANDOFF.md`. Ported onto current main, probes pass; game
   checks and async against sync timings unfinished. The laptop is replay-bound (25-30 ms per frame, item 0b).
2. Texture uploads in gameplay, now the largest cause of long frames: `task/texasync`,
   `build/v121-texasync/HANDOFF.md`. Step 1 splits the cost (image and memory creation, CPU deswizzle, staging,
   copies, locks) before choosing a worker-thread upload, GPU deswizzle, image pooling or a per-frame budget.
3. Virtual-core preemption as Horizon does (MacNativeGames 8bf0893 / e4b4601, PR #488: asynchronous preemption of an
   owner in game code, priority inheritance, the SDK's preemption-disable protocol; Mac worst-1% frame 18.4 to 10.6
   ms): `task/preempt`, `build/v121-preempt/HANDOFF.md`. Port only if the measured waits behind an owner in game code
   are significant; our checkpoints at indirect calls may already cover most.
4. Start-up re-warm when the driver cache does not match the exe path or device (5a.3): `task/rewarm` (main merged
   in, 0b3bc73), untested in game. Needed before the laptop package.
5. Shared shader list: the newer MacNativeGames lists collapse variants that differ only in texture kind into `"*"`
   rows, which our runtime cannot match (it translates for the exact kind). `tools/prepare_shaders.py` reads `"*"`
   as 2D (uncommitted), and `build/shader-lists/union.tsv` unions every list revision since b75be8c. Option: match
   `"*"` rows at run time as MacNativeGames does (`prepare_shader_seed.py:60-76`).
6. Freeze census: a 10-15 minute user session on current main with `TOTK_FLIGHT=1`, `TOTK_TRACE=perf` and
   `TOTK_INPUT_RECORD`, ranking every frame over 50 ms by cause. Decides items 7-9.
7. `TOTK_PIPE_SKIP=fullscreen` as default (cold monsters2: 0 frames over 100 ms, 1% low 33.9 to 41.7 fps; risk: one
   frame without a full-screen pass the first time its pipeline appears). Decide from item 6.
8. ASTC per-frame decode budget (4b): the largest batch, the 169 M texel 8x8 array at boot, scales to about 375 ms of
   GPU time on a GTX 1050 Ti. Split by mips with `minLod` if laptop runs (`TOTK_ASTC_TIME=1`) show decodes in play.
9. ASTC to BC7 re-encode on the GPU for 4 GB cards: the same array is 677 MB as RGBA8 against 42 MB of blocks.
10. Spin-wait gates (MacNativeGames a038e6b, from nxnative 066f2af / 6d34984, MIT): loops polling memory hand the
    core to a queued thread after 50 us. Survey TotK 1.2.1 for such loops first.
11. Over the GPU memory budget, wait for running work and drain deferred releases before failing (MacNativeGames
    4de7b4e); 4 GB cards.
12. Later: recording on several threads, descriptor buffers or bindless (touches the translator), CPU/GPU frame
    overlap as LAS Recomp 13398126d (only with item 1; keeps the phantom-object guard), an x86-64-v2 build for CPUs
    without AVX2/FMA (SavageWindRecomp ae42014b4).
13. Laptop package and test after items 1-5 (GTX 1050 Ti and UHD 630). Untested there: native ASTC on Intel, the
    Intel `minStorageBufferOffsetAlignment` padding of the ASTC path, the legacy-driver vertex-input fallback.

**Bugs**

14. Yunobo fades out and vanishes from the main view about 45 frames after the user presses A (ball fades at
    1335-1350, gone at 1352; his shadow-map draws stay). Repro: `run_game.py build/v121-yunobo --save
    user-save-2026-10-24-yunobo1 --script build/recordings/yunobo-cast2.txt --frames 1360`. Ruled out: occlusion
    counts, CPU readback of GPU targets, missing NVN entry points, GPU-side resources: the game itself stops issuing
    his main-view draws. Leading hypothesis: game logic fades the model alpha (material c11[33].w) and drops it from
    the queues on a CPU input (camera-near fade, GfxColFade, CancelDraw distance or an AI fade). Next: find his c11
    buffer, write-watch c11[33].w to its writer, walk up in Ghidra; cheap check first: fps60 and real-time patches
    off from present 1316. Debug tools uncommitted in `.worktrees/yunobo` (`build/v121-yunobo/debug-tools.diff`);
    detail in `build/v121-yunobo/HANDOFF.md`.
15. Black patches for a moment on the GTX 1050 Ti (item 0a below, `task/alias`, `build/alias-plan.md`).
16. Lantern texture bug (item 6 below): waiting for the user's description or a cropped shot.
17. Validation: `cp_t_tcb_20` R32F view sampled with depth compare in `vkCmdDispatch` (correctness item 8), "VkDescriptorSet
    0x0 does not have a binding for vp_c1" in `vkCmdDrawIndexed` (also in `build/v121/logs/val-base.log`), a fragment
    write to Location 0 with no colour attachment.
18. The correctness queue below.

**Machine and housekeeping**

19. Faulty RAM: about 25 bugchecks in three days, also at 4800 without EXPO; each stick alone passed MemTest86 test
    5. Since 2026-10-25 21:08 one stick (BD100C33) runs in channel A, single-channel, so timings from then on are not
    comparable with dual-channel ones. Next: one stick in slot B; if clean, the pair is the fault (RMA the kit).
    After the fix: clean `gen.py` and `build_game.py`, all diff tests and probes. Zero-filled cache files from the
    2026-10-22 crash are in `build/quarantine-zero-cache-20261025`.
20. Waiting for the user's approval to commit: `docs/backlog.md`, `docs/agent-playbook.md`, `totk.ini.example`,
    `tools/prepare_shaders.py`.
21. Clean up merged worktrees and build dirs (bindskip, astcgpu, pipewait, texbox): notes to `build/archive/`.
22. Paused branches: `task/menu` (in-game menu, 2 commits), `task/v143` (game version 1.4.3, item 22).

## Plan (agreed 2026-10-23)

Work the stages in order. At most three agents at once, at most two of them running the game. Done 2026-10-23 and
removed: validation-layer crash (`build/archive/v121-val`), frame-time summary in the default log
(`build/archive/v121-fts`), shader prebuild (`build/archive/v121-sl`), 1.4.3 recon (`build/v143-recon`), agent docs
and README, settings file `totk.ini` (`runtime/cfg.c`, `--probe-cfg`; a new variable needs a row in `known[]`;
`build/archive/v121-ini`), portable folder (`tools/package_build.py`, README build step 7; `build/archive/v121-port`),
freezes on new pipelines in fights (cold fight 53.9 to 59.7 fps mean, frames over 50 ms 10 to 0;
`TOTK_GPL_DRAW_BUILD=0` turns it off; `build/archive/v121-pfh`), agent tasks in git worktrees (`tools/worktree.py`,
`private-build` skill), rumble strength and curve (`rumble_strength`, `rumble_curve`, `rumble_triggers`), even
present cadence by back-edge pacing (fight intervals outside 16.7 +- 1 ms 24.5% to 2.4%, +3.3 ms latency;
`TOTK_TRACE=present`; `build/archive/v121-pace`), PC keyboard and mouse layout with the mouse camera (`input_map.c`,
`mouse_sens`; `build/archive/v121-kbm`; the game's button actions and the D-pad order 1-4 still to check in game).

**Stage 1: before the tester weekend**

0a. Black screen patches for a moment, seen by the user on the GTX 1050 Ti in the field (2026-10-25). Not the VRAM
    mirror: a verify run that caught the glitch compared 4.45 million mirror pages with 0 mismatches. Leading
    hypothesis: a new texture descriptor over a GPU-rendered texture fails `alias_compatible` and starts as a
    zero-cleared own-memory image (`vk_texture.c` `entry_of`, `create_tex`, `init_image`). The run's two largest
    `left uninitialised` counts (33, 41) fall in its last seconds, where the glitch was. Repro every time: open the
    Purah Pad with a right stick click (keyboard C). Next: `task/alias`, per-alias
    diagnostic lines and a `TOTK_TEX_FILL=magenta` knob, then a user run. Details: `build/alias-plan.md`.
0b. CPU-bound replay on weaker CPUs: with the mirror the 1050 Ti machine (8th-10th gen i7) renders the GPU part in
    10-11 ms, but replay takes 25-30 ms per frame (5.2 ms on the RX 9070 machine), so 25-45 fps. The same machine
    replayed in 7-13 ms at 8 fps in import mode. Mirror bookkeeping explains 2-3 ms. A `sched` run rules out core
    contention: the render thread spends 79-88% of the wall time on the CPU inside `nvnQueueSubmitCommands`; guest threads
    never preempt it and the OS takes about 5%. Suspects: NVIDIA driver cost per command, mirror and texture checks inside replay. Next: a
    render-thread profile from the 1050 Ti (7f2f332 writes it on window close). Details: `build/cpu-plan.md`.
1. User play test of the prepared build: F11, menus, movies, rumble and gyro still to check. First report
   (2026-10-23): flying smooth, short freezes in fights with monsters.
5a. Fight hitches and stutters on new content, the user's main complaint. GPU memory mirror merged (cd6f490,
    `build/mem-plan.md`; 1050 Ti GPU time 110-120 to 10-11 ms). Order now: 2 (ASTC), 3 (re-warm), 1 (replay thread,
    also a candidate fix for 0b). In a warm fight (`fight.txt`, frames 1500+) the game runs 58.3 fps mean,
    52.9% of frames over 16.7 ms, 6.3% over 20 ms; frames over 20 ms are 87% replay (19 ms) with no pipeline or
    shader waits. The user's play added 0 shader translations and 29 full pipelines.
    1. Uneven frames: replay off the critical path. Translate command buffers at `EndRecording` or on a queued replay
       thread, so `QueueSubmitCommands` only queues; `TOTK_REPLAY_ASYNC=0` restores the synchronous path. Merge with
       the new path off by default if the play test leaves any doubt. Code and probes done in `build/v121-rq`
       (0 validation errors); no valid timings yet (its runs fell in the 21:00 file-cache corruption). Next: rebase on
       runtime/, measure `fight.txt` warm with `--quiet`.
    2. Hitches when something new appears: first texture uploads decode ASTC on the CPU inside the frame (367 ms at
       frame 219 of `fight.txt` on the RX 9070; 402-714 ms load frames on the GTX 1050 Ti, `build/mem-plan.md`).
       Desktop AMD and NVIDIA have no ASTC; Intel Gen9-11 iGPUs do (check that `vk_texture.c` uses it when the
       device reports `textureCompressionASTC_LDR`). Plan, after the VRAM mirror:
       1. Measure per load: ASTC bytes, decode time, block-linear deswizzle time (`PERF_ASTC` already times decode).
       2. Licence check: Granite (Themaister, MIT) is believed to ship an ASTC compute decoder; yuzu's
          `astc_decoder.comp` is GPL, read only.
       3. Compute-shader decoder (plain Vulkan 1.1 compute, no extension, so every GPU we run on). Oracle: decode
          every ASTC texture of the 1.2.1 RomFS with it and with `astc_decode_ldr_mt`, bit-exact match required.
       4. In `vk_texture.c`: raw blocks go to staging (4-16x less PCIe traffic), a dispatch writes RGBA8 into the
          image; `TOTK_ASTC=cpu` keeps the CPU path. VRAM use stays as today (decoded RGBA8); GPU re-encode to BC7
          (yuzu's ASTC recompression) only if 2-4 GB cards run out.
    3. Driver pipeline cache after a copy or driver update: the AMD driver keys its cache by exe path, so a moved folder
       stutters until `--prepare-shaders` runs again. `task/rewarm` (1f3cd49, WIP) stamps `shader-prepared.bin` with
       device, driver and exe path and re-warms at start on a mismatch; builds, probes pass, never run in game. Next
       steps in `build/v121-rewarm/HANDOFF.md`. Whether NVIDIA caches pipeline libraries the same way: item 8.
6. Lantern texture bug: waiting for the user's description or a cropped shot.

**Stage 2: tester weekend (2026-10-24/25)**

8. Runs on other GPUs and CPUs (NVIDIA, Vega 7, weaker CPUs, [tester-guide.md](tester-guide.md)). One table: GPU,
   driver, CPU, game version, fps in field and fight, hitches, errors and crashes, with a log per machine (the
   `[host] frametime:` lines give the causes). Check whether NVIDIA and older AMD drivers cache pipeline libraries
   like the RX 9070 does (the drawing thread builds them for a waiting draw, 0.77 ms average here); the AMD driver keys
   that cache by exe path, so each copied folder runs `totk.exe --prepare-shaders` once.

**Stage 3: release next week**

9. Fix what stops play on the tested machines; the rest goes into this file.
10. Commit with the user's approval, GitHub repository, push.
11. Reddit: a post with the repo link (r/decomps, News flair), or the held preview post if the moderators approve it.

**Stage 4: after release, speed and stability**

13. GPU memory over long sessions (details below); 6-8 GB cards are the risk.
16. The correctness queue below, in its order.

**Stage 5: platforms and launcher** (each gets a written design first, agreed with the user)

17. Platform layer: no Win32 outside `runtime/platform/` (`tools/check_platform.py`: 1195 uses in 41 files; FAIL in
    `motion_fusion.c`, `movie_probe.c`, `ov_movie.c`, `swkbd_win.c`, `vk_exec_draw.c`).
18. Linux: `runtime/platform/linux.c`, as SavageWindRecomp splits windows, linux and macos files. Write tracking
    (`host.h` `mem_written_pages`, `mem_freeze`/`mem_thaw`/`mem_frozen`, used by the VRAM mirror and texture checks):
    freezing maps to `mprotect` + a `SIGSEGV` handler; a kernel write (`read()` into guest memory) returns `EFAULT`
    instead of faulting, so the same thaw-before-read sites apply. Written pages: `PAGEMAP_SCAN` ioctl with
    write-protect matching (kernel 6.7+, made for Wine's `GetWriteWatch`); older kernels: write-protect the mirrored
    and texture-bound pages after each sync and mark them in the fault handler (Ryujinx), uploading pools the game
    rewrites every frame whole instead of tracking them. Soft-dirty (`/proc/self/clear_refs`) is process-wide and
    slow, last resort. Under Proton the Windows build already works: Wine emulates both.
19. Steam Deck: RADV offers Vulkan 1.3 and `VK_EXT_external_memory_host`; 4 Zen 2 cores, so expect 30-40 fps.
20. Launcher (details below).

**Later**

21. Button remapping.
22. Game version 1.4.3. Paused 2026-10-23 by the user. Recon in `build/v143-recon` (extracted and verified in
    `build/extracted/v143`; copies to continue from in `build/v143-port`). The generator runs unchanged; estimate one
    day to the title, two to three to play at 60 fps. Steps:
    1. `stg` no-op lift, DT_RELR in `recomp/nso.py` and `runtime/loader.c` (decoder in
       `build/v143-recon/recomp/nso.py`), symbol count from DT_GNU_HASH for sdk/subsdk0.
    2. Jump tables: 26 disagreements and 83 unresolved sites to 0 unexplained, v121 still at 0.
    3. Tools take a version instead of `v121`; re-measure `loader.c:518-526` and `boot.c:22-24`; new boot anchors
       (`boot.c:19-20`).
    4. RELRO (`nn::ro::ProtectRelro`) and the new SDK imports; run to the title.
    5. `recomp/patches-v143.txt` from MNG `game_patches_v143.c` (fps60, bounce, cloth; real time rewritten for the
       `sys+0x1da` coefficient); player position for `frame_rate.c:89`; `make_romfs_patches.py --version`
       (`Common.Product.140`).
    6. Audio REV15 is mostly in place (`audren.c`, `audio_mixer.c`): fix the REV11 self-test; check override
       signatures (biquad, font decode); render differences in play.
23. Community ExeFS patches.
24. Frame rates above 60 and VRR.

## Waiting for a play test

- 60 fps at real-time speed (2026-10-23): MacNativeGames' controller patch, real-time frame step, hair and cloth clock
  and trampoline fix as generator patches (`recomp/patches-v121.txt`, switches `TOTK_FPS60`, `TOTK_REALTIME`,
  `TOTK_CLOTH_REALTIME`, `TOTK_HIGHFPS_FIX`); the pacer ignores the game's interval while the step is real-time.
- Phantom objects and the flat far field (one frame of a rock or log in place of pillars, or a surface filling the
  sky), 2026-10-23. The game's producer rewrites its per-object uniform blocks 14-24 ms after the submit that reads
  them, with no fence; NVN starts the GPU within microseconds, our replay takes longer. A guard in `nvn_api_core.c`
  holds the producer until the newest submit has completed. Item 5a.1 must keep that guarantee.

## Correctness queue

1. Shader census for the open graphics lessons ([port-lessons.md](research/port-lessons.md) G3, G6), offline with no
   game run: how many TotK shaders fetch from buffer textures and how many clears use a partial mask.
2. Image aliasing over GPU-only pool memory (Maxwell 2.3 / check 7, yuzu PR 10418 "areas black for a frame on camera
   changes"): count distinct texture keys per address per frame; flight recorder counts new aliases.
3. Fire on grass deals no damage, grass does not burn (`build/archive/v121-fire2/HANDOFF.md`): the CPU reads GPU-written data in
   GPU-only pools that we never write back (Maxwell 4.1). Recheck after the occlusion merge first.
4. `VK_KHR_index_type_uint8` (check 9): drops the CPU widening of 130-170 byte-index draws per frame.
5. Texture gather rounding on AMD (Maxwell 2.10): yuzu's sub-pixel offset at `ImageGather` fixed BotW shadow edges.
6. Horizon scheduling fidelity beyond the phantom work (port-lessons S1, S3, S5, S6): wake a better-priority thread on
   the signaller's core at SVC return, pin ideal-core -2 and main threads to core 0. Count each case with
   `TOTK_TRACE=svc,sched` first; fight.txt fps must not drop.
7. Line width 4.5 (check 14): dynamic `wideLines`.
8. VUID-vkCmdDispatch-None-06479 (check 20): a D32 copy of the 4x4 R32F placeholder sampled with depth compare.
   Still reported with in-world validation (2026-10-23), together with a fragment write to Location 0 with no
   colour attachment.
12. `vk_memory.c` `defer()` (unverified, `build/archive/v121-val/NOTES.md`): an object destroyed while frame F is
    recorded is stamped with F-1 and may be freed before F's commands that use it are submitted.
9. Masked clears (Maxwell 1.3 / check 3): fix written and parked in `build/archive/v121-clr`; merge if the census finds a
   partial-mask clear.
10. GPU timestamps (port-lessons T1): find whether dynamic resolution reads the report difference before writing real
    GPU times per report.
11. MacNativeGames since our pin `b75be8c` (245 commits, fetched): kernel sync `dab728a` (S6), NVN texture sizes
    `3c482a7`, CPU reads of small render targets `8a29e0a` (item 3), border colour `29dd997`. Add lessons, move the
    pin and fix the `refs/MacNativeGames` line citations in `docs/`.

## Details for later items

- In-game menu, agreed with the user 2026-10-23. Base: LAS Recomp's menu (MIT, same Vulkan base as ours):
  `refs/LASRecomp/runtime/nvn/vulkan/imgui_glue.cpp` (Dear ImGui on our device and dynamic rendering),
  `runtime/ui/menu.mm`, `option_rows.mm`, `controls_ui.mm`; SavageWindRecomp has the same menu. Opens with F1/Esc or
  Minus+Plus. While it is open, the game gets no input and keeps running (no pause). Every setting is saved to
  `totk.ini`. Drawn after the blit of the game image into the swapchain image (`gfx_window.c` around line 536).
  Linux comes later (SDL3 window, item 17): the menu and settings code stays free of Win32; only a thin adapter feeds
  window and input events to ImGui. Menu text goes through a translation catalog (SavageWindRecomp
  `runtime/ui/i18n.mm`), English and Russian. Stages, one branch each:
  1. Frame: ImGui, open and close, input, live settings with `cfg_save`, catalog; pages video (fullscreen and window
     size, FPS counter, frame limit 30 or 60), controls (rumble strength and curve, gyro on or off), sound (volume),
     quit.
  2. The remapping page (`controls_ui.mm`; the bindings of `input_map.c` and
     [controls.md](controls.md) become the defaults) and mouse sensitivity.

  Later pages: AMD FSR 1 or bilinear scaling of the 900p image to the window (LAS Recomp's Windows upscaler); a
  frame-time graph with the wait breakdown of `TOTK_TRACE=perf` (also on F10); VSync (FIFO) for 60 Hz displays
  without VRR, if testers report double frames under back-edge pacing; gyro sensitivity.

  Live settings: today each module reads its `cfg_get` string once and keeps the value (`rumble_cfg` in
  `ov_input.c`). The menu needs `cfg_set` and a version counter that consumers check (cheap; per-frame readers
  re-read only when it changes), and `cfg_save` that rewrites only the changed lines of `totk.ini`, keeps the user's
  comments and adds a missing key under its section (LAS Recomp's `settings_save` rewrites the whole file). A key
  set in the environment shows greyed out in the menu. Settings read only at start (RomFS mods, shader preparation,
  operation mode) are marked "after restart".

  Later: async shader builds, the software keyboard moved onto SavageWindRecomp's `osk.mm`. Rendering above the
  game's 900p is a separate study (a game-side resolution patch, as Switch mods do, or scaling every render target
  ourselves); MacNativeGames has neither (render scale fixed at 1).
- Gyro: the user's logs show only Xbox One and Series X pads, which have no gyro. With a DualSense or Pro Controller,
  check Steam Input (it presents pads as a virtual Xbox pad without sensors) and the game's own motion-aiming option.
- Launcher (plan item 20): one exe for Windows and Linux, SDL3 and Dear ImGui (SavageWindRecomp
  `runtime/ui/launcher.mm`, `tools/fetch_zig.sh`).
  - The player picks the base game (XCI or NSP), the 1.2.1 update and keys. The launcher checks the version,
    extracts, generates and compiles the game code with a bundled compiler (Zig, `zig cc`) and an embedded Python or
    a native generator, then prepares the shaders. About 4 minutes on 12 cores, longer on a Steam Deck.
  - Configuration screen: Play, Settings, Game files, Quit. Settings that need a restart go there.
  - It is the recovery path when a setting stops the boot.
  - The package ships no game code.
- GPU memory over long sessions (port-lessons P3): never measured. MacNativeGames' TotK reached its 5000 MB GPU
  budget after 2 h 43 min (their issue #72) and grew from 4.5 to 6.25 GiB resident; their dead-serial hash sets
  reached ~1 GiB after 3 h. Our textures are destroyed when their pool range goes or a new texture takes the address
  (`vk_texture.c` `entry_of`, `vk_texture_range_gone`), pools at Finalize, Vulkan objects deferred. Started in
  `build/archive/v121-mem` (paused 2026-10-23): a memory line every 1800 frames (VMA heaps, textures, pools, pipelines) and
  a list of growing containers; then a 2-3 h session.
- Attachment feedback loops (`build/archive/v121-fb/HANDOFF.md`, Maxwell 1.9 / check 10): 425-667 draws per frame sample
  G-buffer targets of the open pass, all at the fragment's own pixel. A spec violation with no visible symptom tied to
  it; the v1 fix lost grass (image recreation). Parked until a captured glitch points here.
- Render passes ended by uniform updates and clear-only work (port-lessons P2).
- Map tour (the user's idea): teleport Link across the map by writing position, time of day and weather in guest
  memory, to fill the caches on a first run and to serve as an automatic test (crashes, validation, fps per region).
- Community ExeFS patches (`.ips`, `.pchtxt` for 1.2.1): apply them through the generator's patch table
  (`recomp/patches.py`); RomFS mods already load (`romfs.c`, `TOTK_ROMFS_MODS`). Code-module mods (exlaunch
  `subsdk9`) need porting one by one.
- Faster save loading, asynchronous texture write-back. With 60 fps, keep menus, chests and cutscenes at 30 as
  SavageWindRecomp does (`runtime/hle/framepace.c`; [prior-art.md](research/prior-art.md) section E). A trimmed
  pipeline key (SavageWindRecomp).
- Maxwell sources not read yet (maxwell.md section 8).
