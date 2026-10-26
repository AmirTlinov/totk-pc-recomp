# TotK Recomp

**TotK Recomp** is a static recompilation of *The Legend of Zelda: Tears of the Kingdom* (Nintendo Switch, 2023) into a native Windows x86-64 application.

> [!NOTE]
> Currently supported:
> - Windows 10 or later, x86-64, with a GPU that offers Vulkan 1.3 and `VK_EXT_external_memory_host`
> - Game version **1.2.1** (the update the host runs); the base game as XCI or NSP

> [!IMPORTANT]
> **No game files are included.** TotK Recomp needs your own legally obtained copy of *Tears of the Kingdom*: a base game (1.0.0) and the 1.2.1 update, plus `prod.keys` from your own console. Instructions for obtaining those files will not be provided.
>
> Extraction, the generated C and `totk.exe` happen on your machine and stay out of this repository. The program you build contains the game, so it is for your own use. Do not share it.

The Switch code is lifted from ARM64 to C ahead of time, compiled with Clang, and linked with a host that answers the system calls. NVN, the Switch graphics API, is implemented on Vulkan. Maxwell shaders are translated to SPIR-V the first time the game uses them, then read from a cache. The design is described in [docs/architecture.md](docs/architecture.md).

## Status

This is an early build: version 1.2.1 starts, loads saves and runs in the world at 60 fps with keyboard, mouse or gamepad input and with sound, and it has been tested on AMD, NVIDIA and Intel GPUs. Visual bugs and crashes are expected; the known ones are listed in [docs/backlog.md](docs/backlog.md). No release has been published yet, and macOS, Linux, other game versions and an in-game settings menu are not supported yet.

To report a problem, open an issue and follow [CONTRIBUTING.md](CONTRIBUTING.md). Testers: [docs/tester-guide.md](docs/tester-guide.md) says what to send back.

## Features

- Native code, not emulation: the 1.2.1 `main`, `sdk` and `subsdk0` modules are compiled into `totk.exe`.
- 60 fps with the game running at real-time speed, using MacNativeGames' frame-rate patches (`TOTK_FPS60=0` and `TOTK_REALTIME=0` switch them off).
- Keyboard, mouse and the first gamepad, merged into one controller, with the mouse on the camera and every button remappable: [docs/controls.md](docs/controls.md).
- Rumble with adjustable strength, and motion controls on gamepads that have a gyro.
- Sound through the game's own audio renderer, including voice.
- Language from the Windows locale, docked mode by default, a resizable window, and F11 for borderless fullscreen.
- Shader and pipeline caches, so later launches reuse what the first one compiled.

## How this project is made

Most of the code and documentation here is written by AI coding agents (Claude Code) under the maintainer's direction. The maintainer sets the goals, plays every build, decides what is merged and reviews the result; the agents read the upstream code, write and test the changes and measure them. [AGENTS.md](AGENTS.md) and [docs/agent-playbook.md](docs/agent-playbook.md) are the rules they work under. The same standards apply to every contribution, whoever or whatever wrote it: [CONTRIBUTING.md](CONTRIBUTING.md).

## Documentation

- Players and testers: [controls](docs/controls.md), [tester guide](docs/tester-guide.md).
- How it works: [architecture](docs/architecture.md), with research notes on the upstream projects, Maxwell GPU behaviour and what other ports learned in [docs/research/](docs/research/).
- What is being worked on: [backlog](docs/backlog.md).
- Building: [toolchain-windows.md](docs/toolchain-windows.md), [toolchain-msys2-python.md](docs/toolchain-msys2-python.md), and the [installer](docs/installer.md) that builds the game on a player's machine.
- Coding agents: [AGENTS.md](AGENTS.md) and [docs/agent-playbook.md](docs/agent-playbook.md).

## Building from source

Windows 10 or later, an x86-64 CPU with AVX2, and a GPU with Vulkan 1.3 and `VK_EXT_external_memory_host`. Allow about 60 GB for the work folder. Extraction wants about 55 GB free while it runs and keeps about 35 GB afterwards.

Install the toolchain from [docs/toolchain-windows.md](docs/toolchain-windows.md) and [docs/toolchain-msys2-python.md](docs/toolchain-msys2-python.md): MSYS2 UCRT64 (Clang, Ninja, SDL3, Vulkan, shaderc), Python 3.12 and 3.14, the Vulkan SDK, the .NET 10 SDK, Visual Studio Build Tools, and hactool. A 32-bit `cmake` or `ninja` earlier on `PATH` will be picked up by accident; the build calls the MSYS2 copies itself.

From the repository root, in PowerShell:

1. Python environments (once):

   ```powershell
   uv venv --python 3.12 .venv
   uv pip install --python .venv/Scripts/python.exe -r tools/requirements.txt
   uv venv --python 3.14 .venv-py314
   uv pip install --python .venv-py314/Scripts/python.exe -r tools/requirements-py314.txt
   ```

2. Copy `config.example.toml` to `config.local.toml` and set the paths to your base game, the 1.2.1 update and `prod.keys`. `config.local.toml` is ignored by git. The tools read the keys file themselves.

3. Extract and verify:

   ```powershell
   .venv-py314\Scripts\python tools\extract.py
   ```

   This writes `build\extracted\v0` (base 1.0.0) and `build\extracted\v121` (1.2.1). A hash mismatch stops the run.

4. Shader translator (once; no game files):

   ```powershell
   .venv\Scripts\python tools\build_xlate.py
   ```

   This publishes `build\xlate\totk_xlate.exe`.

5. Lift the 1.2.1 modules, then compile and link:

   ```powershell
   .venv\Scripts\python recomp\gen.py --exefs build\extracted\v121\exefs --out generated\v121 --patches recomp\patches-v121.txt
   .venv-py314\Scripts\python tools\make_romfs_patches.py
   .venv\Scripts\python tools\build_game.py
   ```

   `--patches` builds in the 60 fps and frame-step patches; `make_romfs_patches.py` writes their RomFS half to `build\v121\romfs-patches`. The link copies `SDL3.dll`, `libiconv-2.dll` and `libopus-0.dll` next to the exe.

6. Prepare the shaders (once per build). It needs `refs\MacNativeGames` (`tools\fetch_refs.sh`) for the shared shader list:

   ```powershell
   .venv-py314\Scripts\python tools\prepare_shaders.py build\v121
   ```

   Without this step the first launch compiles shaders and pipelines during play and stutters until the caches fill.

7. Package a portable folder (optional):

   ```powershell
   .venv\Scripts\python tools\package_build.py
   ```

   This writes `build\v121-portable`, which runs on another of your machines without MSYS2, Python or .NET. `romfs.bin` stays out unless you pass `--with-romfs`; otherwise put it in `romfs\` next to the exe or point `TOTK_ROMFS` at its folder. After copying, run `totk.exe --prepare-shaders` once in the new folder, because the driver keeps its pipeline cache per exe path. The folder holds game code and data derived from your game: it is for your own machines only, never share it.

8. Run:

   ```powershell
   build\v121\totk.exe
   ```

   Keep the window in the foreground; an unfocused window stalls the game. Close the window to quit.

`build\v121\totk.exe --selftest` checks the build without game files.

Settings live in `totk.ini` next to `totk.exe`; copy [totk.ini.example](totk.ini.example), which lists them, and edit it. A `TOTK_*` environment variable of the same name overrides the file (`TOTK_FPS60=0` beats `fps60 = 1`). Saves go to `build\profile`, created on first use. Everything the build writes stays under `build\` and `generated\`, which git ignores.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) first. Nothing that came out of the game goes into the repository: no game files, keys, extracted data, generated C, binaries, shaders or caches. `tools/check_content.py` is what CI runs, and it is what you run before a commit.

## Credits

This project stands on two MIT codebases and the projects they came from. Code taken from them keeps its license; the copies are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

- **[LAS Recomp](https://github.com/peiikai/LASRecomp)** by Peiikai (MIT) is a static recompilation of *Link's Awakening* for Windows, macOS and Linux. The lifter, the FP/SIMD lowering, the Windows memory layer and the differential tests start there.
- **[MacNativeGames](https://github.com/ly05010419/MacNativeGames)** (MIT) runs *Tears of the Kingdom* natively on Apple silicon. The game-specific host behaviour, the NVN front end, the version tables, the extraction checks and the shader translator's patches come from there.
- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** and **[nxnative](https://github.com/seedds/nxnative)** (MIT) are the Horizon host and NVN layer that MacNativeGames grew from.
- **[Ryujinx](https://github.com/Ryujinx/Ryujinx)** (MIT) provides the Maxwell shader translator and the audio-effect algorithms.
- **[hactool](https://github.com/SciresM/hactool)** by SciresM (ISC) extracts NCA and update containers.
- **[SDL 3](https://libsdl.org)** (zlib) is the gamepad and the audio output.
- **[libopus](https://opus-codec.org)** (BSD) decodes voice.
- **[libvpx](https://chromium.googlesource.com/webm/libvpx)** (BSD) decodes the cutscene movies.
- **[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)** (MIT) allocates GPU memory.
- **[deko3d](https://github.com/devkitPro/deko3d)** (zlib) is the source of the block-linear image layout, via MacNativeGames, and a reference for Maxwell command semantics.
- **[SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp)** (MIT), a static recompilation of *Breath of the Wild*, is the model for preemption at indirect calls and a reference for frame pacing and the platform split.
- The Switch homebrew community's documentation ([Switchbrew](https://switchbrew.org)).

Research notes on each of them: [docs/research/](docs/research/).

## Legal

TotK Recomp is an independent, non-commercial fan project. It is not affiliated with, endorsed by or sponsored by Nintendo. *The Legend of Zelda*, *Tears of the Kingdom* and *Nintendo Switch* are trademarks of Nintendo.

No game files, game code, assets, shaders, fonts or keys are included in this repository. You must provide your own legally obtained copy to play. The executable you build from that copy contains the game. Keep it to yourself.

## License

The project's own code is under the [MIT License](LICENSE). The license covers this project's code only and grants nothing for Nintendo's game, which you supply yourself.

Third-party code keeps its own license. The full list, with the commit each copy was taken from, is [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). A local build links or ships these pieces (versions are the MSYS2 UCRT64 packages the toolchain doc was written against; a newer package from the same source is fine):

| Component | Version | License | How it is used |
|---|---|---|---|
| SDL 3 | 3.4.18 | zlib | `SDL3.dll` next to `totk.exe` (gamepad, audio) |
| libiconv | 1.19 | LGPL 2.1+ | `libiconv-2.dll`, unmodified, a dependency of SDL 3 |
| libopus | 1.6.1 | BSD 3-clause | `libopus-0.dll` next to `totk.exe` (voice) |
| libvpx | 1.17.0 | BSD 3-clause, with the WebM patent grant | linked statically into `totk.exe` (movies) |
| Vulkan Memory Allocator | 3.4.0 | MIT | compiled into `totk.exe` |
| Ryujinx shader translator | 1.3.2 | MIT | `build\xlate\totk_xlate.exe`, a separate process |
| GCC runtime (`libstdc++`) | 16.2.0 | GPL 3.0 with the GCC Runtime Library Exception | linked into `totk.exe` |
| winpthreads (mingw-w64) | 14.0.0 | MIT / BSD | linked into `totk.exe` |

Unicorn (GPL 2.0) stays a separate test tool for the lifter. hactool (ISC) and Capstone (BSD) run at build time.
