# Agent playbook: running, measuring and debugging the game

Read this before you run the game, take a frame dump or measure performance. This page holds the conventions; the
`--help` of each tool holds its options.

## Work in a task worktree

Start, build, hand back and clean up a task with the `private-build` skill (`.claude/skills/private-build/SKILL.md`): a git worktree on `task/<tag>` and a build dir `build/v121-<tag>`.
The pipeline cache, `pipeline-keys.bin`, `shader-cache`, `shader-programs.bin` and `shader-answers.bin` live next to
each `totk.exe`, so a task build starts cold: seed it by copying `build/v121`'s five before timing anything. A build seeded
from an older one stalls on pipeline and shader builds, and its slow frames say nothing about the change. (Until 2026-10-22 every task build wrote into
`build/v121`'s caches, and task builds broke each other's runs.)

## Running

Run a build into gameplay with the `run-game` skill (`.claude/skills/run-game/SKILL.md`): `tools/run_game.py`, the save
backups and their scenes, input scripts. `build/profile` is the user's real profile: never write it. Other agents run
the game at the same time: stop only a `totk.exe` you started, by PID; `taskkill /IM totk.exe` closes every instance.

## Measuring

Timings are only comparable on a quiet machine. Pass `--quiet` to take the machine-wide lock `build/.gpu-run.lock`
(held until the game exits, freed by the OS if the caller dies, holder pid and build in the file), then wait until no
other `totk.exe` or `ninja.exe` runs. The log's first and last lines record the count either way.

- `--env TOTK_TRACE=perf,sched`, then `python tools/perf_summary.py <log> <lo> <hi>`: fps, max frame and the
  render-thread buckets per 60 frames. Per-thread run, core, host, lock and addr shares come per 120 presents.
  A second `perf: frame N: gpu avg` line per window has the GPU busy time per frame (timestamp queries per submission
  part, `vk_sync.c`; upload batches not included), fence waits of the render thread and of other threads, VRAM use and
  budget (`VK_EXT_memory_budget`), process RAM and `bound: gpu|cpu|pace` (pace: the frame waited for pacing 15% of the
  time or more; gpu: the GPU was busy 80% of the interval or more).
- The pool census (`mem: census`, `vk_memory.c` `vk_mem_census`) is logged at frame 1800, with `perf` every 1800
  frames: pools by kind and flags, and the bytes bound or copied per frame by pool class since frame 1200.
- `TOTK_PROFILE=1 TOTK_PROFILE_FRAMES=lo-hi`: sampling profiler (`runtime/prof_win.c`). Resolve `exe+0x...` RVAs
  with the build's `totk.map`.
- `tools/regress.py <build> --base build/v121-m4 [--routes fight,rock,island,field] [--repeat 3] [--cold] [--validation]
  [--out dir]`: runs the routes through `run_game.py --quiet`, scores logs and `TOTK_SHOTS` frames
  (`tools/regress_score.py`, tested by `tests/regress_score_test.py`) and prints median and worst per metric. Run the candidate alone and judge it against
  the targets: max fps, no slow frames (`perf: slow frame`), no black or flat frames, no new validation errors.
  `--base` (a diff with `ok`, `better`, `worse`) only when a question needs a side-by-side; do not rerun a base build
  by habit. Glitches are found by looking at the shots and dumps, not by the diff. `--cold` empties only the
  pipeline cache. Each route writes `<out>/<build>/<route>-worst.png` (the 6 worst frames) and `route_failed` marks a
  run that ended in game over, crashed or stopped early: its numbers do not count. One run takes 2 to 4 minutes.
- Baselines (Ryzen 9 7900X, RX 9070, 1600x900, 60 fps patches and real-time step on, 2026-10-23):
  - field: 59.4 fps mean, 60 median, replay 9.5 ms per frame;
  - `fight.txt`: 59.1 fps mean, 60 median, replay 13.6 ms, 0 CPU-side slow frames; the slow frames left are
    pipeline and shader-translation waits.
- `--validation` runs work in the world (1800 frames checked). The layer prints each message kind 10 times, so it
  gives no totals; the known messages left are in the backlog correctness queue, item 8.

## Frame dumps

`TOTK_DUMP=<frame> TOTK_DUMP_DIR=build/captures/<name>` writes per-pass PNGs, `passes.txt` and `images.txt`
(`runtime/vk_dump.c`). `passes.txt` lists bound textures with swizzles, storage buffers with their c0 slots, and
copies and clears. `TOTK_DUMP_BUF=addr:size[:off:stride:w:h]` dumps a guest buffer.

- A draw's argument lines come before its pass header.
- Pass numbers shift between runs. Find a pass by its draw count instead: the g-buffer pass has about 1990 draws.

RenderDoc cannot capture: the driver hides `VK_EXT_external_memory_host`.

## Other GPUs

The RX 9070 is the only GPU here; the build also has to run on NVIDIA and older AMD (Vega, wave64 only). New Vulkan code
queries the feature, extension, format or limit it uses and keeps a working path or a logged fallback when it is
missing (`vk_pipeline.c` provoking vertex and subgroup size pinning are examples). Never hard-code what this GPU
reports: alignments, subgroup size, depth formats, `maxPushDescriptors`.

## Checks before handing back

`totk.exe --probe-kernel`, `--probe-vkexec`, `--probe-nvn-tex` and `--probe-shader` run from the build directory and
must pass. CPU-header changes also need `tests\diff\simd.py`, `lift.py` and `func.py` (AGENTS.md).
`tools\build_game.py` hashes every object against `obj/objects.sha256` before it links and recompiles a mismatch (the
faulty RAM flipped bits in stored objects once, `build/BOOTCRASH.md`). The line `objects verified: N, recompiled after
a hash mismatch: M` should show M of 0; a nonzero M means rerun the probes. A task build checks the shared objects against
the main build's manifest, so build `build/v121` first.

## Bug classes already fixed

Check these classes before you re-investigate a symptom that looks like one of them.

- A colour target outside the fragment shader's output map must keep its texel, as on Maxwell: write mask 0 from
  `vk_pipeline.c` `target_wmask`. AMD wrote undefined values there (lime silhouettes in grass); zero-filling it
  instead wiped the half-res depth (sky rectangles).
- The CPU copies GPU-rendered textures between pools with memcpy (grass tile maps): `nvn_api_core.c` `ov_memcpy`
  writes them back first. Other CPU reads of GPU output are not covered yet. A write-back skips a texture whose
  storage the CPU rewrote after the GPU render (`vk_texture.c` `write_back`); writing it anyway put stale grass maps
  into the far terrain for a second (texture flicker on a hit).
- The CPU reads GPU-rendered textures in CPU-cached pools: `nvnMemoryPoolInvalidateMappedRange` writes them back
  (`nvn_ext.c` `nvn_pool_invalidate`; TotK invalidates its 260x260 R16 grass maps and 1x1 targets), and once the CPU
  waits for a fence, targets up to 64 KiB rendered before it go back too (`vk_texture.c`
  `vk_texture_write_back_small`): the 128x128 cut map is read with no invalidate. Without it a cut left the grass
  standing, with no blades and no insects.
- A one-shot draw (a target not rendered in the previous frame) waits for its shader and pipeline instead of being
  skipped: `vk_exec_draw.c` `prepare_draw`. Skipping left the cloud and fog volumes empty for the session.
- Storage buffers bound by Ryujinx first-use order instead of the NVN c0 slot: `vk_exec_draw.c` `ssbo_slot`. It showed
  as tile rectangles.
- 3D colour targets rejected: `vk_exec.c` `resolve_target`. The grading LUT was never written, so the dialog
  background and the loading map were black.
- Primitive restart ignored: `vk_exec_draw.c` `restart_for`. It showed as streaks on cliffs.
- The slot-follow path picked the newest texture at shared storage: `vk_texture.c` `vk_texture_by_id` now prefers
  `same_but_swizzle`. It showed as a streaky map screen.

## Lessons from other ports

Before you form a hypothesis about a symptom, follow "Before you investigate a symptom" in
[research/port-lessons.md](research/port-lessons.md): evidence first, then a search of the lessons and the upstream
notes. `refs/nxnative/docs/agents/05-debugging.md` has recipes for intermittent glitches, racy crashes and memory growth.

## Context budget

Read output through a filter; the full text stays on disk.

- Game logs run to megabytes: `grep` the lines you need, `tail -20` for the end, `perf_summary.py ... | grep SUMMARY`
  for timings.
- Generated C (`generated/v121`) runs to hundreds of thousands of lines per file: `grep -n` the function, then
  `sed -n` its range.
- Look at screenshots downscaled: paste several into one 1280x720 grid with PIL and read the grid.
- Long-running commands go to the background with their output in a file; read the file's last lines when it ends.
- Research notes (`docs/research/`) run to 40 KB each: `grep -n` the symptom or the subsystem, then read that
  section.
- Read each screenshot once; write what it shows into the notes instead of opening it again.
- Keep notes in `build/v121-<tag>/NOTES.md` from the first finding, so a handoff costs one write.

After about 40 tool calls (a call costs about 2.4k tokens of context), write `build/v121-<tag>/HANDOFF.md` (under 80 lines) and stop. Include:
findings with file paths, what is ruled out, the leading hypothesis, the exact next step, and the temporary debug edits
(file:line). A fresh agent continues from it.
