---
name: private-build
description: Fork a private build of the runtime to change and test it without touching runtime/ or other agents' builds, then hand back a diff. Use before editing runtime/ or tools/ code, building totk.exe, or cleaning up a finished build dir.
---

# Private build: fork, build, hand back, clean up

Several agents change `runtime/` at once and the coordinator merges their diffs, so every change starts in a fork:
`build/v121-<tag>/`, where `<tag>` is a short name for your task.

## 1. Fork

Copy `runtime/` to `build/v121-<tag>/runtime-src` once and edit only there. For a `tools/` change, copy the file into
`build/v121-<tag>/` and run the copy. `generated/v121` and `recomp/` are shared: report a needed lifter change and leave
it unmade.

## 2. Build

```
export PATH=/c/msys64/ucrt64/bin:$PATH && .venv/Scripts/python tools/build_game.py --runtime build/v121-<tag>/runtime-src --out build/v121-<tag>
```

The fork reuses `build/v121`'s generated objects: it hardlinks them into its `obj/` and compiles only `runtime/*.c`, so
a first build takes about 10 s. It does so when `build/v121` is up to date and every header the generated code includes
is byte-identical in your `runtime-src`. If you changed such a header (`cpu.h`, ...) or the main build is stale, it
prints `compiling all generated C: <why>` and rebuilds everything (about 200 s on every core). `--full-build` forces
that. A `.c` change is incremental either way. After every build check the exe's mtime: a failed build leaves the old
`totk.exe` in place, and you would test old code.

Run the game with the `run-game` skill and the probes from the build dir: `totk.exe --probe-kernel`, `--probe-vkexec`,
`--probe-nvn-tex`, `--probe-shader` must pass before you hand back.

## 3. Hand back

A fix that costs frame time is not done: keep new work off per-draw and per-pixel paths unless the semantics need it,
and say in the report what the change costs per draw, shader or frame. The coordinator measures `fight.txt` with
`--quiet` before merging.

A change on the render path also gets one look: a field `--shot` of your build next to one of `build/v121` at the same
frame. Runs diverge, so small differences prove nothing, but gross regressions show (grass gone, colours off, black
areas) and block the hand-back.

Write `build/v121-<tag>/<tag>.diff` with `diff -u --strip-trailing-cr runtime/<f> build/v121-<tag>/runtime-src/<f>` for
every changed file, against `runtime/` as it is just before you write it (others merge meanwhile). Keep LF line
endings. Read the diff's `-` lines: each must be a line you meant to change, not a merge you are reverting.

Keep notes in `build/v121-<tag>/NOTES.md` as you go. When you stop unfinished, write `build/v121-<tag>/HANDOFF.md`
(under 80 lines): findings with paths, what is ruled out, the leading hypothesis, the exact next step, temporary debug
edits (file:line).

## 4. Clean up

After the coordinator merges the diff, or drops the task, the fork goes: copy `*.md`, `*.diff` and any helper scripts
to `build/archive/v121-<tag>/`, then delete `build/v121-<tag>/`. Forks of tasks waiting to be continued stay until
then. `build/v121` (the main build) and `build/v121-pf` (shared shader and pipeline caches) are never deleted.
