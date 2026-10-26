# Toolchain: MSYS2 UCRT64, Python, Switch content tools

Set up 2026-10-20. Package set follows `refs/LASRecomp/tools/setup_windows.ps1` plus cmake, ninja, gcc, git, unzip.
`setup_windows.ps1` itself was not run.

## MSYS2 UCRT64 (`C:\msys64`)

Update: `C:\msys64\usr\bin\bash.exe -lc "pacman -Syuu --noconfirm"` (twice: first pass updates the core and exits).

Install:

    pacman -S --needed --noconfirm make mingw-w64-ucrt-x86_64-{clang,lld,sdl3,opus,libvpx,ffmpeg,vulkan-devel,shaderc,
      spirv-tools,pkgconf,gdb,vulkan-memory-allocator,vulkan-validation-layers,nasm,cmake,ninja,gcc,make} base-devel git unzip zip p7zip

| Package | Version |
|---|---|
| clang / lld | 23.1.3 |
| gcc | 16.2.0 |
| make (msys) / mingw32-make | 4.4.1 |
| sdl3 | 3.4.18 |
| opus | 1.6.1 (`libopus-0.dll`, voice) |
| libvpx | 1.17.0 (`libvpx.a`, linked statically for the movies) |
| ffmpeg | 9.0.2 (never linked: the MSYS2 build is GPLv3) |
| vulkan-devel group (headers, loader, utility libs, spirv-headers, spirv-tools, validation layers) | 1.4.363 |
| shaderc (glslc) | 2026.4 |
| vulkan-memory-allocator | 3.4.0, header at `/ucrt64/include/vma/vk_mem_alloc.h` |
| pkgconf | 3.0.9 |
| cmake / ninja | 4.4.4 / 1.13.2 |
| nasm / gdb | 3.02 / 18.1 |
| git / unzip / zip / p7zip | 2.56.0 / 6.0 / 3.0 / 17.06 |

Use from Git Bash: `export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"`. `tools/build_game.py` copies
`SDL3.dll`, `libiconv-2.dll` and `libopus-0.dll` next to `totk.exe`, so the game runs without MSYS2 on `PATH`.

Verify:

    clang --version; ld.lld --version
    pkgconf --modversion sdl3 vulkan opus vpx
    glslc --version; ffmpeg -version; spirv-val --version

### Vulkan back end

    # all part of the install line above plus spirv-cross and glslang; --needed makes this a check
    pacman -S --needed mingw-w64-ucrt-x86_64-{vulkan-devel,vulkan-memory-allocator,vulkan-validation-layers,shaderc,spirv-tools,spirv-cross,glslang}

Verify:

    pkgconf --modversion vulkan                      # 1.4.363
    spirv-val --version; spirv-dis --version
    ls /ucrt64/include/vma/vk_mem_alloc.h /ucrt64/lib/libvulkan-1.dll.a
    vulkaninfo --summary                             # Vulkan SDK; lists the discrete GPU, Vulkan 1.3 or newer

`tools/build_game.py` links `-lvulkan-1 -luser32 -lgdi32 -ldwmapi` and adds `/ucrt64/include/vma` to the runtime include
path (`#include "vk_mem_alloc.h"`). At run time `vulkan-1.dll` comes from `System32`.

## Python

uv 0.13.3 at `%USERPROFILE%\.local\bin\uv.exe` (standalone installer, not winget).

| venv | Python | Purpose | Pins |
|---|---|---|---|
| `.venv` | 3.12.15 (uv-managed) | lifter / generator (capstone, unicorn, lz4, keystone, lief, pyelftools), oead 1.3.0, evfl, quicktex, PyYAML, Pillow, numpy, cryptography, nsz, zstandard, pyinstaller | `tools/requirements.txt` |
| `.venv-py314` | 3.14.7 (system) | MacNativeGames TotK scripts: they use stdlib `compression.zstd` (3.14+) | `tools/requirements-py314.txt` |

Version pins taken from LASRecomp `Makefile` (capstone 5.0.9, unicorn 2.1.4, lz4 4.4.5, pyinstaller 6.22.3) and
`refs/MacNativeGames/games/totk/requirements*.txt` (cryptography 50.0.2, nsz 5.0.0, Pillow 12.3.0, brotli 1.2.0).
`capstone.__version__` reports `5.0.7` although the installed wheel is 5.0.9; the wheel's metadata is right.

Recreate:

    uv venv --python 3.12 .venv
    uv pip install --python .venv/Scripts/python.exe -r tools/requirements.txt
    uv venv --python 3.14 .venv-py314
    uv pip install --python .venv-py314/Scripts/python.exe -r tools/requirements-py314.txt

Verify:

    .venv\Scripts\python -c "import capstone, unicorn, lz4; print(capstone.__version__, unicorn.__version__)"
    .venv\Scripts\python -c "import capstone; md=capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); print([i.mnemonic for i in md.disasm(bytes.fromhex('1f2003d5'),0)])"
    # -> ['nop']
    .venv-py314\Scripts\python -c "import compression.zstd, capstone, unicorn, lz4, cryptography, nsz"

## Switch content tools (`toolchain\`)

| Tool | Path | Status |
|---|---|---|
| hactool 1.4.0 @ `1d64a83450e025622f3468c28fc4164dad2c5ef6` (the MacNativeGames pin) | `toolchain\hactool\hactool.exe` | built, static, depends only on system DLLs |
| hactoolnet | - | not installed: `github.com/Thealexbarney/LibHac` returns 404 (repo, releases and assets gone) |

hactool source: `toolchain\src\hactool` (git checkout of the pinned commit). Build: UCRT64 gcc, `config.mk` =
`CC=gcc EXEEXT=.exe CFLAGS=-O2 ... LDFLAGS=... -static`, `make -j8`.

Applied the portable part of MacNativeGames `build_app.py:build_hactool()` (kept as
`toolchain\hactool\portable-patches.diff` in the local toolchain folder, not in the repository):

1. `MNG_TITLE_KEYS` env var selects the title-key file.
2. BKTR: allow 0x200-aligned padding after the subsection table (`nca.c`).
3. BKTR: accept the subsection total-size layout of newer cumulative updates (`nca.c`).

Ours (kept locally as `toolchain\hactool\unicode-patches.diff`): `hactool.manifest` with `<activeCodePage>UTF-8</activeCodePage>`,
embedded through `hactool.rc` (Makefile rule `hactool.res.o`, `windres`). hactool converts argv, `HOME` and
`USERPROFILE` from UTF-8 to UTF-16 (`filepath.c`); without the manifest they arrive in the ANSI code page, and a
non-ASCII user profile or path stops it with `Failed to convert ... to UTF-16!`. Needs Windows 10 1903 or later.

Rebuild: in an MSYS2 UCRT64 shell (`MSYSTEM=UCRT64 C:\msys64\usr\bin\bash.exe -lc ...`) in `toolchain\src\hactool`,
`make clean && make -j8`, then copy `hactool.exe` to `toolchain\hactool\`. gcc writes temporary files to `TMP`; where
it cannot (sandboxed shells), point `TMP` and `TEMP` at a writable folder.

Not applied: the `MNG_BASE_RANGE_OFFSET/SIZE` base-file hook. It uses `funopen` (BSD/macOS only); on Windows the
base NCA must be extracted to a file first.

Verify: `toolchain\hactool\hactool.exe` prints `hactool (c) SciresM.` and usage.

Keys: hactool reads `-k <prod.keys>`, else `$HOME\.switch\prod.keys` (falls back to `%USERPROFILE%` when `HOME` is
unset; MSYS shells set `HOME`, so pass `-k` explicitly). Use your own keys; no keys are in this repo.
