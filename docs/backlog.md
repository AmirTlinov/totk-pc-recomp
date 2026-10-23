# Backlog: the plan and the proposed fixes

Priorities: no artifacts and no crashes first, then 60+ fps without dips, then other GPUs and platforms. Each item
names its evidence; Maxwell item numbers refer to [research/maxwell.md](research/maxwell.md). Remove an item when it is
merged or dropped.

## Plan (agreed 2026-10-23)

Work the stages in order. At most three agents at once, at most two of them running the game. Done 2026-10-23 and
removed: validation-layer crash (`build/archive/v121-val`), frame-time summary in the default log
(`build/archive/v121-fts`), shader prebuild (`build/archive/v121-sl`), 1.4.3 recon (`build/v143-recon`), agent docs
and README, settings file `totk.ini` (`runtime/cfg.c`, `--probe-cfg`; a new variable needs a row in `known[]`;
`build/archive/v121-ini`), portable folder (`tools/package_build.py`, README build step 7; `build/archive/v121-port`),
freezes on new pipelines in fights (cold fight 53.9 to 59.7 fps mean, frames over 50 ms 10 to 0;
`TOTK_GPL_DRAW_BUILD=0` turns it off; `build/archive/v121-pfh`).

**Stage 1: before the tester weekend**

1. User play test of the prepared build: F11, menus, movies, rumble and gyro still to check. First report
   (2026-10-23): flying smooth, short freezes in fights with monsters.
5a. Fight hitches, the user's main complaint. In a warm fight (`fight.txt`, frames 1500+) the game runs 58.3 fps mean,
    52.9% of frames over 16.7 ms, 6.3% over 20 ms; frames over 20 ms are 87% replay (19 ms) with no pipeline or
    shader waits. The user's play added 0 shader translations and 29 full pipelines.
    1. Uneven frames: replay off the critical path. Translate command buffers at `EndRecording` or on a queued replay
       thread, so `QueueSubmitCommands` only queues; `TOTK_REPLAY_ASYNC=0` restores the synchronous path. Merge with
       the new path off by default if the play test leaves any doubt. Code and probes done in `build/v121-rq`
       (0 validation errors); no valid timings yet (its runs fell in the 21:00 file-cache corruption). Next: rebase on
       runtime/, measure `fight.txt` warm with `--quiet`.
    2. Streaming: the slow loading frames are first texture uploads (ASTC, 367 ms at frame 219 of `fight.txt`).
       Measure where the time goes, then decode on the GPU or ahead of use.
6. Lantern texture bug: waiting for the user's description or a cropped shot.
7a. Keyboard and mouse: the PC layout and mouse camera agreed with the user (details below). Sonnet.

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
18. Linux: `runtime/platform/linux.c`, as SavageWindRecomp splits windows, linux and macos files.
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

- Keyboard and mouse (plan item 7a, agreed 2026-10-23). Today the keyboard map is `input_win.c:34-36` (camera on
  I J K L, attack on F) and there is no mouse input at all.
  - Layout:

    | Switch | Action in the world | Keys |
    |---|---|---|
    | left stick | move | W A S D |
    | right stick | camera | mouse; arrow keys as a second source |
    | Y | attack | left mouse button |
    | ZL | shield, lock on | right mouse button, also Z |
    | ZR | bow | F, also X |
    | X | jump | Space |
    | B | run, cancel | Left Shift |
    | A | interact, pick up | E |
    | L | abilities | Q |
    | R | throw weapon | R |
    | left stick click | crouch | Left Ctrl |
    | right stick click | | C |
    | D-pad | | 1 2 3 4 |
    | Plus | inventory | Tab |
    | Minus | map | M |

    Esc stays free for the in-game menu. Z and X duplicate ZL and ZR so menu pages turn with two neighbouring keys;
    menu tabs (L, R) are Q and R. The mouse wheel stays unbound: L and R are abilities and weapon throw in the world.
  - Before coding, check in the game what L/R, ZL/ZR, X/Y, the D-pad and the right stick click do in the inventory
    and in the world, and correct the action column.
  - Mouse camera: raw mouse deltas per frame become right-stick deflection, with sensitivity
    (`TOTK_MOUSE_SENS`) and the larger-deflection rule against the gamepad. The cursor is hidden and confined to the
    client area while the window is focused and released on focus loss and while F11 changes the window. Not true
    mouselook: the game's stick curve and dead zone still apply.
  - Update [controls.md](controls.md). Remapping goes into `totk.ini` (plan item 21).

- In-game menu (Dear ImGui over our Vulkan output, as SavageWindRecomp `runtime/ui/menu.mm`). Opens with F1/Esc or
  Minus+Plus. While it is open, the game gets no input. Pages:
  - fullscreen and window size;
  - performance overlay on F10 (fps, or a frame-time graph with the wait breakdown of `TOTK_TRACE=perf`);
  - volume;
  - async shader builds;
  - quit.

  Settings live in `totk.ini`. Leave room for later pages: upscaler, frame-rate choices, remapping,
  and the software keyboard moved onto SavageWindRecomp's `osk.mm`.
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
