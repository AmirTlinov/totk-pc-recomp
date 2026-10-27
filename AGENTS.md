# TotK Recomp

Static recompilation of *Tears of the Kingdom* (Switch, ARM64) into a native Windows x86-64 exe with a Vulkan renderer. Plan: [docs/architecture.md](docs/architecture.md).

## Game content boundary

This repository is public. Game data lives outside it and every derived file stays in ignored folders.

- The user's game and key paths are in `config.local.toml` (ignored). Pass the keys file path to tools; the tools read it themselves. Key values stay out of your output, logs, commands and files.
- Everything extracted or generated goes under `build/` or `generated/`.
- Every file the code generator emits carries the marker defined as `GENERATED_MARKER` in `tools/check_content.py`.
- Run `python tools/check_content.py --staged` before proposing a commit. It is the same check CI runs.

## Reusing code

`refs/` holds upstream checkouts at the commits the research notes cite (`tools/fetch_refs.sh` recreates it). Read-only.

- Copy only MIT, BSD, ISC, zlib or Apache-2.0 code, keep its header, and add a row to `THIRD_PARTY_NOTICES.md`.
- yuzu, suyu, OpenSmash-Ultimate and other GPL code: read for behaviour, write our own.

## Research notes

Read the note before working on its area; each cites `file:line` in `refs/`.

- Lifter, generator, Windows memory layer, differential tests: [docs/research/lasrecomp.md](docs/research/lasrecomp.md)
- TotK host behaviour, NVN, shaders, extraction, version tables, patches: [docs/research/macnativegames.md](docs/research/macnativegames.md)
- Other ports and recompilers: [docs/research/prior-art.md](docs/research/prior-art.md)
- Sizes, function and instruction counts, OS touch-points of the 1.2.1 modules: [docs/research/totk-v121-inventory.md](docs/research/totk-v121-inventory.md)
- Maxwell GPU semantics our Vulkan path must match, TotK bugs other ports and emulators fixed: [docs/research/maxwell.md](docs/research/maxwell.md)
- Symptoms other ports already solved (artifacts, crashes, pacing, scheduling) and whether we have the fix; search it before investigating any symptom: [docs/research/port-lessons.md](docs/research/port-lessons.md)

## Toolchain

Setup and verify commands: [docs/toolchain-windows.md](docs/toolchain-windows.md), [docs/toolchain-msys2-python.md](docs/toolchain-msys2-python.md).

- Build C with MSYS2 UCRT64 Clang (`C:\msys64\ucrt64\bin`). Generated C uses GNU extensions, so `cl.exe` is out.
- A 32-bit winlibs `cmake`/`ninja` sits early on the user PATH. Call CMake and Ninja from `C:\msys64\ucrt64\bin` by full path or put that directory first.
- Two Python environments: `.venv` (3.12) for the lifter and LAS Recomp tools, `.venv-py314` for MacNativeGames' extraction scripts, which need `compression.zstd`.
- Game extraction: `.venv-py314\Scripts\python tools\extract.py` writes verified ExeFS and RomFS to `build/extracted/v0` (1.0.0) and `build/extracted/v121` (1.2.1, the primary target). `tools/vendor/` stays byte-identical to upstream; Windows changes go in `tools/extract.py`.
- Lifter (`recomp/`) and CPU headers (`runtime/`) start from LAS Recomp. Every lifter change passes the differential tests against Unicorn, all run with `.venv`:
  - `tests\diff\lift.py`: instructions sampled from the extracted 1.2.1 modules (one per opcode signature, then random); `--coverage` lists every word the lifter rejects or turns into a trap.
  - `tests\diff\simd.py`: the whole FP/SIMD encoding space; needs no game files.
  - `tests\diff\atomic.py`: exclusive load/store sequences and ARMv8.1 atomics; needs no game files.
  - `tests\diff\func.py`: whole leaf functions as the generator emits them, loops and jump tables included.
  - `tests\excl_mt.py`: the exclusive monitor helpers of `cpu.h` under contention and ABA, on several threads; needs no game files.
- Game code build (`.venv`): `recomp\gen.py --exefs build\extracted\v121\exefs --out generated\v121 --patches recomp\patches-v121.txt` writes the C (about 15 s). The patch table holds the 60 fps and real-time frame step code patches, switchable at run time (`recomp/patches.py`); `.venv-py314\Scripts\python tools\make_romfs_patches.py` writes their RomFS half to `build\v121\romfs-patches`. `tools\build_game.py` compiles and links `build\v121\totk.exe` with Ninja (about 200 s clean, incremental after). The AMD driver cache that makes prepared shaders fast is keyed to the exe path and holds a limited amount: preparing another build dir can evict this one's libraries (40-220 ms per new effect in play); `totk.exe --warm-shaders` rebuilds them from the shader cache (seconds when they are still cached). `recomp/layout.py` alone decides module bases. Jump tables are resolved by `recomp/jumptables.py`; after changing it, `tests\jumptables_check.py` must report 0 disagreements with its Unicorn oracle (on 1.4.3 too: `--exefs build\extracted\v143\exefs`).
- Game version 1.4.3: the same commands with `v143` (`--patches recomp\patches-v143.txt`, `build_game.py --version v143`, `make_romfs_patches.py --version v143`). Its builds default to `build\profile-v143`; `run_game.py` runs it on copies of the 1.2.1 save backups.
- Shader census (`.venv-py314`): `tools\shader_census.py --version v121 [--translate]` extracts every Maxwell shader to `build/v121/shaders/` and runs it through `build\xlate\totk_xlate.exe` and `spirv-val`; failure groups go to `failures.txt`. `tools\xlate_smoke.py` checks the translator alone, without game files.
- Shader preparation (`.venv-py314`, once per build dir and translator): `tools\prepare_shaders.py build\v121` translates every game shader's default variant and the variants of the MacNativeGames shared shader list (read from `refs/` at run time, with the rows its history dropped: `tools/shader_list.py`) into the build's shader cache and builds their stage libraries into the driver cache (`totk.exe --prepare-shaders`; 1.4.3: 29,587 variants, 7 min on 24 threads, 1.7 GB); play then translates and compiles only variants neither covers.
- Open work with its evidence: [docs/backlog.md](docs/backlog.md).
- Installer (`installer/`, `tools/make_release.py`), which builds the game on a player's machine: [docs/installer.md](docs/installer.md).
- Running, measuring or frame-dumping the game, save backups, task worktrees and their builds: [docs/agent-playbook.md](docs/agent-playbook.md).
- Project skills (`.claude/skills/`): `private-build` before changing runtime/ or tools/ code and building totk.exe (one git worktree per task); `run-game` to run a build into gameplay from a save.
- hactool: `toolchain/hactool/hactool.exe`. From an MSYS2 shell pass `-k <keys>` explicitly; the shell's `HOME` hides the default key location.
- Ghidra 12.1.2 with SwitchLoader: `toolchain/ghidra/ghidra_12.1.2_PUBLIC`. Game modules (NSO) open here, headless or GUI.
- REA (`reverse-engineer-anything` skill + MCP server) analyses our own x86-64 builds. On Windows its Ghidra provider admits PE files only, so NSO analysis goes through Ghidra directly. Register the MCP server once per clone, at local scope:

  ```powershell
  claude mcp add --scope local rea -e "GHIDRA_INSTALL_DIR=<repo>\toolchain\ghidra\ghidra_12.1.2_PUBLIC" -e "JAVA_HOME=<jdk-21>" -- cmd.exe /d /c npx -y rea-agents@6.7.0 mcp
  ```

## Writing

Code, comments, docs and commit messages are in English. Commit messages are plain sentences without type prefixes.
