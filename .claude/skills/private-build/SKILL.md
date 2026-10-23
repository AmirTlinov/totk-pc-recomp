---
name: private-build
description: Run a task in its own git worktree and build dir, commit on task/<tag>, hand back a branch. Use before editing runtime/ or tools/ code, building totk.exe, or cleaning up a finished task.
---

# Task worktree: start, build, commit, hand back, clean up

Several agents change `runtime/` and `tools/` at once and the coordinator merges their branches, so every task gets a
git worktree `.worktrees/<tag>` on branch `task/<tag>` and a build dir `build/v121-<tag>`. `<tag>` is a short name for
your task.

## 1. Start

```
.venv/Scripts/python tools/worktree.py new <tag>
```

Work and build only inside `.worktrees/<tag>`. The worktree holds tracked files. `build/`, `generated/`, `refs/`,
`toolchain/`, `.venv/`, `.venv-py314/` and `config.local.toml` are links to the main checkout, so everything written
there is shared: `generated/` and `recomp/` belong to the coordinator. A needed lifter or generator change is reported
in the hand-back, never made. `tools/` changes are made in the worktree like any other file.

## 2. Build

```
export PATH=/c/msys64/ucrt64/bin:$PATH && .venv/Scripts/python tools/build_game.py --runtime runtime --out build/v121-<tag>
```

The build reuses `build/v121`'s generated objects: it hardlinks them into `obj/` and compiles only `runtime/*.c`, so a
first build takes about 10 s. It does so when `build/v121` is up to date and every header the generated code includes
is byte-identical in your `runtime/`. If you changed such a header (`cpu.h`, ...) or the main build is stale, it prints
`compiling all generated C: <why>` and rebuilds everything (about 200 s on every core). `--full-build` forces that. A
`.c` change is incremental either way. After every build check the exe's mtime: a failed build leaves the old
`totk.exe` in place, and you would test old code.

Run the game with the `run-game` skill. `totk.exe --probe-kernel`, `--probe-vkexec`, `--probe-nvn-tex` and
`--probe-shader` pass before you hand back.

## 3. Commit

Commit on `task/<tag>` as often as you like.

- Run `python tools/check_content.py --staged` before each commit.
- Messages are plain English sentences, no type prefix, no `Co-Authored-By` trailer.
- `main` stays untouched: you never commit to it, merge into it or push anything.

Keep notes in `build/v121-<tag>/NOTES.md` as you go. When you stop unfinished, write `build/v121-<tag>/HANDOFF.md`
(under 80 lines): findings with paths, what is ruled out, the leading hypothesis, the exact next step, temporary debug
edits (file:line). Both stay uncommitted.

## 4. Hand back

A fix that costs frame time is not done: keep new work off per-draw and per-pixel paths unless the semantics need it.
The coordinator measures `fight.txt` with `--quiet` before merging.

A change on the render path also gets one look: a field `--shot` of your build next to one of `build/v121` at the same
frame. Runs diverge, so small differences prove nothing, but gross regressions show (grass gone, colours off, black
areas) and block the hand-back.

Then:

1. Merge or rebase `main` into `task/<tag>` until your branch merges into `main` cleanly. Rebuild and rerun the four
   probes on the result.
2. Report: the branch name, the commit list (`git log main..task/<tag> --oneline`), what each change costs per draw,
   shader or frame, the probe results, and any lifter or generator change you need.

## 5. Clean up (coordinator)

After the merge, or when the task is dropped:

1. Copy `*.md` and any helper scripts from `build/v121-<tag>/` to `build/archive/v121-<tag>/`.
2. `.venv/Scripts/python tools/worktree.py remove <tag> --delete-branch` (it unlinks the links before git removes the
   worktree, refuses a dirty worktree or an unmerged branch; `--force` overrides).
3. Delete `build/v121-<tag>/`.

Tasks waiting to be continued keep their worktree and build dir. `build/v121` (the main build) and `build/v121-pf`
(shared shader and pipeline caches) are never deleted.
