# Installer

A public release that turns the player's own game files into a playable TotK folder. The release carries no game
data and no code derived from the game: the installer builds the game on the player's machine. The installed folder
holds only the game; the installer and its tools stay outside it.

## What the player does

1. Downloads `TotK-Setup-<version>-windows.zip` from GitHub Releases, unpacks it anywhere and starts `TotK-Setup.exe`.
2. Picks:
   - the base game (XCI or NSP) and the 1.2.1 update (NSP);
   - their `prod.keys`;
   - the game language and the install folder.
3. Waits for the setup, one time. The window shows each step and its progress.
4. Plays `totk.exe` from the install folder. The installer and the source NSP or XCI can be deleted.

The install folder (`main.c` `portable_paths()` reads this layout; it stays portable):

```
<install folder>/
  totk.exe, SDL3.dll, libopus-0.dll, totk_xlate.exe
  totk.ini                       language; the other keys as commented defaults (totk.ini.example)
  exefs/, romfs/                 extracted from the player's files
  profile/                       saves
  shader and pipeline caches     prepared by the setup
  totk-install.json              what built this folder: release version, input hashes, finished steps
```

## Updating the port

The player runs a newer installer on the same folder. It reads `totk-install.json`, keeps `romfs/`, `exefs/`,
`profile/` and `totk.ini` after checking their hashes, rebuilds `totk.exe` and re-prepares the shaders. A newer
release that changes nothing game-derived skips the rebuild.

## Setup steps

1. **Check** before writing anything: the files open with the keys, the update is 1.2.1 (by hash, as
   `tools/extract.py`), the keys are new enough for the update, about 22 GB is free on the target disk, the CPU has
   AVX2 (`-march=x86-64-v3`), a Vulkan 1.3 GPU exists. Each failure is one clear message.
2. **Extract** ExeFS and RomFS into the install folder and verify the hashes.
3. **Lift** the ExeFS to C (`recomp/gen.py`, `recomp/patches-v121.txt`) into a temp folder next to the installer.
4. **Compile** the generated C, `modules.c` and `romfs.c` (they bake per-install layout and RomFS hashes) and link
   them with the prebuilt runtime library into `totk.exe` (`build_game.py --runtime-lib`, proven on task/llvmmingw:
   3 min on 24 cores).
5. **Prepare** the RomFS patches and the shaders (`tools/make_romfs_patches.py`, `tools/prepare_shaders.py`,
   `totk.exe --prepare-shaders`) in the install folder.
6. **Clean up** the temp folder (generated C and objects, about 1.3 GB).

A cancelled or failed setup resumes from the last finished step (`totk-install.json`).

## Release layout

The unpacked release; `tools/make_release.py` assembles it, CI zips it:

```
TotK-Setup.exe        starts python/pythonw.exe -m installer (a small launcher)
installer/            the installer: steps, CLI, window, en/ru text catalog
python/               Python 3.14.9 embeddable + numpy, lz4, cryptography, cffi, pycparser (+ tkinter for the window)
recomp/, tools/       the lifter and the build, extraction and shader scripts the steps run (copied from the repo)
llvm/                 llvm-mingw, x86_64-only subset (Apache-2.0 with LLVM exception; mingw-w64 parts permissive)
runtime/              libtotkrt.a and the headers the generated code and modules.c/romfs.c include
bin/                  totk_xlate.exe, hactool.exe, SDL3.dll, libopus-0.dll, ninja.exe
data/                 shader_list.tsv (MacNativeGames, MIT)
THIRD_PARTY_NOTICES.md
```

- SDL3 comes from the upstream release, not MSYS2: MSYS2's DLL imports the LGPL libiconv.
- Every bundled third-party piece gets a row in `THIRD_PARTY_NOTICES.md`.
- The release contains nothing game-derived; `tools/check_content.py` runs over it in CI.

## Constraints

- The installer steps contain no Win32 calls: Linux follows with its own compiler and the same steps.
- The keys file path is passed to the tools; key values never appear in the UI, logs or files.
- The window is a front end over the CLI (`python -m installer --base ... --update ... --keys ... --lang ... --dest
  ...`), which prints one JSON progress event per line. Tests and CI use the CLI.
- Unsigned exe: SmartScreen warns on first start.

## Later

- Re-warm the driver cache on start after a move, GPU or driver change (WIP on task/rewarm: stamp in
  `shader-prepared.bin`).
- NSZ and XCZ input. Linux release.
