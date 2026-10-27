# Port lessons: symptoms other ports already solved

Each lesson maps a player-visible symptom to the cause another port found, and records whether our runtime has the fix.
Sources: nxnative (MIT, `refs/nxnative` at `d5b380b`, 2026-10-22; `docs/agents/03-bringup.md`, `04-graphics.md`,
`05-debugging.md`, `docs/development.md`, the TotK profile `dm/games/0100F2C0115B6000/game.tsv`) and MacNativeGames
([macnativegames.md](macnativegames.md)). nxnative runs TotK 1.4.3, so its `main+` addresses do not carry over to 1.2.1.
Maxwell semantics live in [maxwell.md](maxwell.md); a lesson that is also a Maxwell check names its number (`mx N` =
maxwell.md section 7 check N). Our side was checked by reading the code on 2026-10-22 and updated 2026-10-26. Our code is cited by file and function
or SVC number, so the citations survive edits; `file:line` citations point into `refs/`.

Status: **open** (missing or partial), **unknown** (not checked), **done**, **parked**.

## Before you investigate a symptom

1. Get the evidence first: the user's screenshot or recording, or a flight-recorder dump
   ([agent-playbook.md](../agent-playbook.md), "Frame dumps"). A guess without a captured frame costs a game run each.
2. Search the lessons for the symptom words: this file, maxwell.md sections 6-7, then the upstream notes:
   `grep -rniE 'black|flicker' docs/research refs/nxnative/docs refs/MacNativeGames/docs`. Words that match well are
   what the player sees (black, magenta, flicker, slow motion, missing, stale, streaks) and the object (sky, grass,
   shadow, fog, HUD).
3. A **done** match: confirm our fix still holds before deriving anything new. An **open** match is the first
   hypothesis.
4. Read the code the hypothesis depends on before any game run: the guest functions in Ghidra or `generated/v121`,
   our runtime path. A hypothesis the code already rules out costs no run (2026-10-23: ten agent runs went to
   scheduler and lock theories that one Ghidra read of the game's double-buffer protocol would have narrowed).
5. Then falsify what is left with the cheapest switch, dump or trace before writing a fix. A fix that changes timing
   or scheduling also needs the slow-frame count of the same route next to the run without it.

## Keeping the lessons current

Upstream changes daily. `git -C refs/<name> fetch origin`, then `git log --format='%h %s' <pin>..origin/HEAD` lists the
new commits and `git diff <pin> origin/HEAD -- docs` their notes. Add lessons here, then move the pin in
`tools/fetch_refs.sh`. nxnative rewrote its history on 2026-10-22; when a pinned hash is gone, find it by commit subject.

## Open: timing and frame pacing

| id | symptom | cause and their fix | our status | next step |
|---|---|---|---|---|
| T1 | GPU-bound scenes run in slow motion; dynamic resolution never drops | The Zelda engine's GPU time is the `TIMESTAMP` report at the end of the frame minus `TIMESTAMP_TOP` at its start, read after the frame fence; the interval controller and dynamic resolution use it. One time per submission made it 0-6 ticks (BotW). Fix: each report gets the GPU start or end of its command segment (`gpu_timestamps segment`, `DM_NVN_GPU_TS`; 04-graphics.md) | open: every report gets the host time at fence completion (`vk_exec_xfer.c` `write_counter`), so the difference is microseconds. Counter word 0 is right (conditional clears need it). 1.2.1's interval controller does not read it: `[x1+0x118]` (`0x8f26c8`) is `GetSystemTick` minus the frame start (`f_70008f24bc`); dynamic resolution not checked | find a reader of the report difference; then `vkCmdWriteTimestamp` (top of pipe for `TIMESTAMP_TOP`) converted to 19.2 MHz ticks with calibrated timestamps |
| T2 | frame-rate counter wanders 50-60 | the vsync event and the window acquire each kept a 60 Hz clock and drifted apart; both follow one display tick now (03-bringup.md) | open: `gfx_window.c` `vsync_main` and the pacer of `nvn_api_obj.c` (`pace`, `present_period`) run separate clocks | derive acquire slots from the vsync thread's tick |
| T3 | a 30 Hz thread runs at 27 Hz, slow cutscenes | host sleep overshoot; `sleep_precise` | partly: sleeps up to 1 ms spin (`kernel.c` `sleep_ns`; 20 us sleeps took 460-515 us before, [windows-host.md](windows-host.md)); longer ones use the high-resolution waitable timer, 0.3-0.5 ms late | measure `SleepThread` at 5, 16 and 33 ms with `TOTK_TRACE=sched` |

## Open: thread scheduling

Horizon runs a better-priority thread at once and pins some threads to one core; a host runs them all in parallel.
A one-step ordering difference between two engine threads can show as one frame with inconsistent data.

| id | symptom | cause and their fix | our status | next step |
|---|---|---|---|---|
| S1 | state read one step early; use-after-free between threads | a woken better-priority thread pinned to the signaller's core runs before the signaller's next instruction (`dm_yield_to_core_peers`, `core_preempt`; 03-bringup.md) | open: the preempt flag is honoured at the holder's next SVC or indirect call (`dispatch.c` `dispatch_preempt`; MacNativeGames `threads.c:120-151` checks at SVCs), and a waiter interrupts a holder in game code after 250 us (S7), but a woken thread does not run before the signaller's next instruction. Tried 2026-10-23: a check at every host call made phantom rocks rarer (0/10 to 4/6 clean runs) but added 50-75 ms CPU-side hitches, so it was reverted; a nxnative-style waker handoff did not help | none until a symptom is traced to it alone |
| S2 | per-core data races, different thread order | `CreateThread` with ideal core -2 and the main thread get the default core and a mask of that core only; `SetThreadCoreMask` -2 means core 0 / mask 1 | open: mask 7 for negative cores and the main thread (`kernel.c` SVC 0x08 `CreateThread` and the main thread's `thr_new`); `SetThreadCoreMask` -2 keeps the old ideal core (`kernel.c` SVC 0x0F) | count -2 creations with `TOTK_TRACE=svc`, then pin |
| S3 | worker threads burn cores, late frames | 843 SVC-free spin loops in TotK `main`; nxnative's rewriter makes them poll 2 ms, then sleep | partly: a better-priority waiter takes the core at the spinning holder's next indirect call or interrupts it after 250 us (S7); a spin loop still burns a host core and keeps equal-priority waiters out | measure core-wait while a holder spins (sched trace `preempt` latency) |
| S4 | rare hang or lost wake | `SignalToAddress` type 2, count <= 0 with waiters: Ryujinx, yuzu and nxnative set `value - 2` | done: `kernel.c` SVC 0x35 writes `value - 1` and the count > 0 branch writes `nw <= count ? value - 1 : value`, both as mesosphere master `kern_k_address_arbiter.cpp`; 1.2.1 calls type 2 only with count 1, and `value - 2` did not change the phantom rocks (2026-10-23) | none |
| S5 | the main thread holds a core a worker needs | yield types 0 / -1 / -2 differ on Horizon; TotK's render-graph wait is a poll plus yield | unknown: all three hand the core only to an equal or better waiter (`kernel.c` SVC 0x0B) | trace which yield type 1.2.1's frame-end poll uses |
| S6 | lost or extra wakes | MacNativeGames `dab728a`: `SignalToAddress` wake-all, `SystemEvent` rebinding, targeted wakes | unknown | read the commit, compare with `kernel.c` |
| S7 | stutter: a better thread waits milliseconds for a core whose owner runs game code | MacNativeGames 8bf0893 (PR #488, on by default e4b4601): the waiter signals the owner, which gives the core up when interrupted in top-level game code; priority inheritance through guest mutexes; the SDK's preemption disable (TLR+0x100 / +0x102, SVC 0x36); without the last two their game hung at "Continue". 1.1.2: 58% of long core waits were this class; Mac worst-1% frame 18.4 to 10.6 ms | done: merged, on by default (`TOTK_ASYNC_PREEMPT`; SuspendThread and a register-saving stub, `kernel.c` `kern_ap_stub`, instead of a signal). `TOTK_CORE_DIAG=1` measured this class at 0.47 / 0.83 / 1.06 ms of thread-time per frame (field / fight / monsters2) with our indirect-call checkpoints, waits to 16+ ms; with the port 0.29 / 0.34 / 0.84 and the waits of 1 ms or more 53 / 41 / 27 to 4 / 10 / 18. 1.2.1's SDK has the disable protocol (sdk f_70051242a8, f_7005124360). A first stub clobbered the flags before saving them (`sub` before `pushfq`) and crashed at boot | A/B timings: [backlog](../backlog.md) item 11 |

## Open: graphics

| id | symptom | cause and their fix | our status | next step |
|---|---|---|---|---|
| G3 | light or decal lists, particle data read 0 | buffer textures read by 1D integer fetches; bound as 2D rows | open: buffer textures are not replayed and bind a stand-in (`vk_exec_draw.c` `bind_resources`) | census; Vulkan texel buffer views need no 2D trick |
| G4 | textures read as the dummy | bindless handles stored in the game's own uniform buffers (`tex_ubo`) | not in 1.2.1: 0 of 75k descriptors take a handle from cbuf >= 3 and no startup shader has a `_B` texture op (2026-10-22); only c2 handles are mapped (`tools/xlate/Program.cs` `TexSlot`) | if a later version needs it: nxnative `ubo_read` (`dm/gfx/nvn/nvn_mtl.m:4199-4215`) |
| G5 | garbage in feedback effects | new GPU textures start at zero | open: depth starts cleared, GPU-only colour images start undefined (`vk_texture.c` `init_image`; mx 7) | clear new GPU-only colour images |
| G6 | 50x50 black tiles, masks not cleared | clears with a partial channel or stencil mask | parked: a fix was written and not merged (mx 3, backlog item 29) | merge if the census shows partial-mask clears |
| G7 | garbage geometry on title or load screens | 8-bit indices written by a copy in the same submission read stale by CPU widening | open: CPU widening at record time (`vk_exec_draw.c` `op_draw_idx`; mx 9) | `VK_KHR_index_type_uint8` (backlog item 24) |
| G8 | cave stone blocks get no torch light | `ReadTexels` of a CPU-filled BC4 atlas returned the never-uploaded copy | open: ported, not checked on the 5120x5120 atlas | dump the cave save |
| G9 | scattered squares in albedo | G-buffer attaches one texture at two slots | unknown | validation on a G-buffer frame |
| G10 | stale 32x32 blocks where pre-pass and G-buffer disagree | identical position math must give identical depth (invariance) | unknown | grep a dumped VS SPIR-V for `Invariant` |
| G11 | characters stretched into streaks | integer vertex inputs vs the format's type | unknown | count `VUID-vkCmdDraw-Input-08734` |
| G12 | one flat ambient colour | a 1-layer 2D array sampled as 2D | unknown | read view creation in `vk_exec_draw.c` `get_view` |
| G13 | magenta streamed textures | sparse textures use a 64 KB tile layout | unknown | look for virtual pools in the NVN log |
| G14 | upside-down UI text | lower-left window origin | unknown: origin recorded (`nvn_api_core.c` `nvnDeviceSetWindowOriginMode`) | grep the NVN log for the origin |

## Open: performance

| id | symptom | cause and their fix | our status | next step |
|---|---|---|---|---|
| P2 | low fps outdoors | inline uniform updates and clear-only passes end render passes; deferring them cut TotK's attachment traffic from 1,100 to 700 MB a frame (`DM_NVN_UB_PARTIAL`, `DM_NVN_LAZY_CLEAR`, `cb_extent.tsv`) | open: `UpdateUniformBuffer` is a buffer copy between full barriers, which end the pass (`vk_exec_draw.c` `op_update_ub`) | count passes per frame and those ended by an update in a `TOTK_DUMP` |
| P3 | out-of-memory after minutes | dead textures re-entered tables | unknown | log VMA bytes and live images every 1000 frames |

## Open: debugging tools

| id | lesson | our status |
|---|---|---|
| D1 | Per-shader highlight, depth override and skip (`DM_NVN_HILITE`, `DEPTHOVR`) split "rejected by depth" from "never drawn" and bisect a glitch to one shader | open: `TOTK_DUMP` has none |
| D2 | First-use texture info with the count of non-zero guest bytes (`TEXINFO`, `TEXRAW`, `ZBLOCK`) shows a texture the CPU filled but we did not upload | open: dump lines carry address, mirror state and upload count only |
| D3 | Crash reports with a stack scan and the words at `*xN` | open: vectored handler (`main.c` `crash_handler`) |

## Done

Confirm the fix still holds before re-deriving any of these.

- Speed follows the present interval at 60 fps (T4): MacNativeGames' controller patch and real-time frame step as
  generator patches (`recomp/patches-v121.txt`, `runtime/frame_rate.c`); the pacer ignores the game's interval while
  the step is real-time (`nvn_api_obj.c` `present_period`).
- Driver constant bank c0 (G2, fog that never parts, flicker in tile rows): a c0 bank per stage with the dispatch
  size at 0x200, point size at 0x610 and base instance at 0x644, `vk_exec_draw.c` `vk_c0_fill`.
- GPU idle while the replay records (P1): the replay is submitted in parts between passes every `TOTK_SPLIT_DRAWS`
  (64) draws, only the last part signals, `vk_exec.c` `maybe_split`.
- Unwritten vertex outputs read (0,0,0,1) (no sun shadows in Link's Awakening): translator patch
  `tools/xlate/patches/0006-initialise-unlinked-vertex-outputs.patch` (mx 2).
- Conditional clears on counter words; a timestamp report writes 0 in the counter word: `vk_exec_xfer.c`.
- CPU-filled textures without a flush (magenta cave walls): write-watched pool mirrors, `vk_memory.c` (`mem_written_pages`) and `vk_texture.c`. The same
  path covers BotW's sky (tables memcpy'd into a CPU-cached pool, flushed, sampled from a zero texture: brown sky).
- Deferred texture binds read at execution (3D scene black): `nvn_api_cmd.c` `Bind*Deferred`.
- `nvnTextureCompare` compares the channel mapping (HUD prompt boxes, black stamina wheel): `nvn_api_obj.c` `nvnTextureCompare`.
- BC textures drawn through an RGBA16UI view (BC4 encoder): block-texel-view-compatible images, `vk_texture.c` `block_views_ok`.
- Fragment draws with no render target: `vk_exec.c` `resolve_rt`. Output types from attachment formats: `vk_pipeline.c`
  `vk_pipeline_answers`.
- Block-height rounding: `nvn_tex.c`, the block-height pick (mx 15). Depth mode: `vk_pipeline.c` `create_graphics` (mx 16).
- Same-pass feedback sampling (magenta or dark 16x16 blocks, G15): feedback-loop layout and a by-region barrier, or a pass
  restart without the extension (`vk_exec_draw.c` feedback loops, `TOTK_FEEDBACK`; mx 10).
- Cubes sampled as cube arrays, 1D textures, swizzles, sRGB views, scaled copies, targets outside the output map keep
  texels: see the playbook's "Bug classes already fixed".
- `GetCommandMemoryUsed` follows the driver's sizes and resets at `AddCommandMemory`: `nvn_api_cmd.c` `nvnCommandBufferGetCommandMemoryUsed`. The game
  sizes model heaps from it, so a larger host record crashes world loads.
- `nvnQueueFinish` waits for the GPU only: `nvn_api_core.c` `nvnQueueFinish`.
- Yield back-off: none (`kernel.c` SVC 0x0B); one that starts before 2 ms of yields drops TotK to 20 fps.
- Condition-variable waiters wake in arrival order within a priority: `kernel.c` (`kern_test_cv_old_order` restores the old order
  for tests).
- The guest clock never freezes: `main.c` `rt_ticks`. Focus message 15 only under focus modes 1 and 2: `gfx_window.c`
  `WM_ACTIVATEAPP`, `ov_system.c` `SetFocusHandlingMode`.
- Alias region and heap: `memory.c` (`HEAP_CAP`, `ALIAS_SIZE`). Adr-relative jump tables: `recomp/jumptables.py`.
- Scripted and recorded input by frame: `TOTK_INPUT_SCRIPT`, `TOTK_INPUT_RECORD`.

Not applicable to us: Metal and iOS limits (big-stack entry points, TBDR store order, encoder order, display-link rates,
efficiency cores), the NSO rewriter's retry blocks, `DT_RELR` (SDK 20 only), GC-title thread contexts, audio renderer
take-over (we serve `audren:u`).
