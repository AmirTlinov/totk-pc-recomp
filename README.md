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

The Switch code is lifted from ARM64 to C ahead of time, compiled with Clang, and linked with a host that answers the system calls. NVN, the Switch graphics API, is implemented on Vulkan. Maxwell shaders are translated to SPIR-V the first time the game uses them, then read from a cache.

## Status

This is an early build. Version 1.2.1 starts, shows the title screen, loads saves and runs in the world at 60 fps, with keyboard or gamepad input and with sound. Graphics, stability and speed are still being worked on: visual bugs and crashes are expected. You build it from source; nothing is packaged for download.

It is developed and measured on one machine (Ryzen 9 7900X, Radeon RX 9070). NVIDIA and older AMD GPUs are expected to work but are barely tested; [docs/tester-guide.md](docs/tester-guide.md) says what to send back.

To report a problem, open an issue and follow [CONTRIBUTING.md](CONTRIBUTING.md). Include the GPU and driver, the log, and the steps that get there. Screenshots are fine.

### Known problems

- For one frame, a rock or pillar can appear in the wrong place, or nearby objects vanish, most often while running or falling from a sky island. The leading suspect is thread ordering the game expects from the Switch scheduler; see [docs/backlog.md](docs/backlog.md).
- Without `tools\prepare_shaders.py` (build step 6) the first launch compiles shaders and pipelines and stutters until the caches fill. With it, a fight on a cleared driver cache had 1 slow frame instead of 37.
- Newly streamed areas still have single slow frames while their textures upload.
- Far terrain can turn flat gray or brown for a few frames after Link is knocked down.

### Not in yet

- macOS and Linux
- Running 1.0.0, 1.1.2 or 1.4.3 (the extractor recognises those updates; the host is built for 1.2.1)
- Button remapping
- A package that builds the game code on a player's machine. Today you compile it yourself

## Features

**Runs natively**
- **This is not emulation.** The 1.2.1 `main`, `sdk` and `subsdk0` modules (about 96 MB of image, a little under 300,000 functions) are compiled into `totk.exe`.
- Title screen, save loading and play, including the overworld, combat and the map.
- 60 fps with the game running at real-time speed: MacNativeGames' 60 fps and frame-step patches are built in and can be switched off (`TOTK_FPS60=0`, `TOTK_REALTIME=0`).
- F11 switches between the window and borderless fullscreen.
- Keyboard and the first SDL3 gamepad, merged into one controller. Buttons are taken by position, so the south face button is B. The key map is in [docs/controls.md](docs/controls.md). Keys count only while the game window is in front.
- Sound through the game's own audio renderer, played with SDL3, including Opus voice.
- Language from the Windows locale, or from `TOTK_LOCALE`. A locale the game does not ship becomes `en-US`. `TOTK_LANG` sets a Switch code directly (`en-US`, `ja`, `zh-Hant`, …).
- Docked mode by default, which reports the boost performance configuration. `TOTK_OPERATION_MODE=handheld` reports the normal one.
- A resizable window that opens at 1920×1080. The game's picture is scaled to the client area.
- Presentation uses mailbox when the driver offers it, and vsync (FIFO) otherwise. `TOTK_VK_PRESENT=fifo` or `immediate` selects those.
- A shader cache and a Vulkan pipeline cache. The first launch translates and compiles; the next launch reuses both.
- Saves under `build/profile`, created on first use.

## How it works

The design and the research it rests on are in [docs/architecture.md](docs/architecture.md).

1. **Input.** Your base game and the 1.2.1 update are decrypted and unpacked locally with your own `prod.keys`. File hashes are checked against known retail dumps.
2. **CPU.** `main`, `sdk` and `subsdk0` are lifted to C, one function per guest function. NEON and floating point are lowered to C that follows ARM rounding. Guest addresses are host addresses. Jump tables are resolved by a symbolic pass in the generator.
3. **Host.** Threads, memory, the RomFS, saves, time, account, language and the audio renderer are implemented on Windows. Every guest thread is a host thread, but only three run game code at once: the host models the Switch's three application cores and its priority preemption, because the game keeps per-core data without locks and orders some work by thread priority alone. Input and audio output go through SDL3.
4. **Graphics.** The NVN front end records what the game submits. A Vulkan 1.3 back end replays it, uploading the memory the CPU wrote and writing GPU results back when the CPU reads them. The Ryujinx Maxwell translator, built as `totk_xlate.exe`, turns shaders into SPIR-V. Pipelines are linked from precompiled stages on first use (`VK_EXT_graphics_pipeline_library`) and optimised in the background.

## How this project is made

Most of the code and documentation here is written by AI coding agents (Claude Code) under the maintainer's direction. The maintainer sets the goals, plays every build, decides what is merged and reviews the result; the agents read the upstream code, write and test the changes and measure them. [AGENTS.md](AGENTS.md) and [docs/agent-playbook.md](docs/agent-playbook.md) are the rules they work under. The same standards apply to every contribution, whoever or whatever wrote it: [CONTRIBUTING.md](CONTRIBUTING.md).

## Documentation

- Players and testers: [controls](docs/controls.md), [tester guide](docs/tester-guide.md).
- How it works: [architecture](docs/architecture.md), with research notes on the upstream projects, Maxwell GPU behaviour and what other ports learned in [docs/research/](docs/research/).
- What is being worked on: [backlog](docs/backlog.md).
- Building: [toolchain-windows.md](docs/toolchain-windows.md), [toolchain-msys2-python.md](docs/toolchain-msys2-python.md).
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

   `--patches` builds in the 60 fps and frame-step patches; `make_romfs_patches.py` writes their RomFS half to `build\v121\romfs-patches`. The generator takes well under a minute. A clean compile takes a few minutes and is incremental after that. The link copies `SDL3.dll`, `libiconv-2.dll` and `libopus-0.dll` next to the exe. `vulkan-1.dll` comes from Windows.

6. Prepare the shaders (once per build; about 5 minutes and 1.3 GB). It needs `refs\MacNativeGames` (`tools\fetch_refs.sh`) for the shared shader list:

   ```powershell
   .venv-py314\Scripts\python tools\prepare_shaders.py build\v121
   ```

7. Package a portable folder (optional; about 2.4 GB without the RomFS):

   ```powershell
   .venv\Scripts\python tools\package_build.py
   ```

   This writes `build\v121-portable` with `totk.exe`, its DLLs, the shader translator (a native exe, no .NET), the ExeFS, the RomFS index and patches, the shared fonts and the shader and pipeline caches. Copy the folder to another machine of yours; it needs no MSYS2, Python or .NET, only Vulkan 1.3 drivers. `romfs.bin` stays out (16 GB): `--with-romfs` copies it, or put it in `romfs\` next to the exe, or set `TOTK_ROMFS` (also settable in `totk.ini`) to its folder. Saves go to `profile\` in the folder (`TOTK_PROFILE_DIR` moves it, `TOTK_EXEFS` moves the ExeFS). The folder holds game code and data derived from your game: it is for your own machines only, never share it. The driver keeps its pipeline library cache per exe path, so after copying run `totk.exe --prepare-shaders` once in the new folder (about a minute; it needs no Python), or the first fights stutter.

8. Run:

   ```powershell
   build\v121\totk.exe
   ```

   Keep the window in the foreground; an unfocused window stalls the game. Close the window to quit. The first launch spends time on shaders and pipelines. Later launches read `build\v121\shader-cache` and `build\v121\pipeline-cache.bin`.

`build\v121\totk.exe --selftest` checks the layout and the host tables and needs no game files. `--frames N` exits after N presented frames; `--frames N --shot out.png` writes that frame. The other modes are development probes (`--boot`, `--probe-kernel`, `--probe-fs`, and the rest printed by the exe when the arguments are wrong).

Where files live, all under the repository and all ignored by git:

- Extracted code and data: `build\extracted\v121\exefs` and `build\extracted\v121\romfs`
- Generated C: `generated\v121`
- The program: `build\v121\totk.exe`, with the three DLLs beside it
- Saves, temp and cache: `build\profile` (`--profile` on `totk.exe` or on `tools\build_game.py` points elsewhere)
- Shader cache: `build\v121\shader-cache` (`TOTK_SHADER_CACHE` overrides the directory)
- Pipeline cache: `build\v121\pipeline-cache.bin` (`TOTK_PIPELINE_CACHE` overrides the file)
- Translator: `build\xlate\totk_xlate.exe` (`TOTK_XLATE` overrides the path)

Settings live in `totk.ini` next to `totk.exe`; copy [totk.ini.example](totk.ini.example) and edit it. A `TOTK_*` environment variable overrides the file (`TOTK_FPS60=0` beats `fps60 = 1`), and the startup log lists the file read and each override. The `TOTK_*` names in this README are also the ini keys without the prefix.

`TOTK_AUDIO=none` mixes without opening a device. `TOTK_GPU=null` runs the graphics front end with no GPU, for tests.

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
| Vulkan Memory Allocator | 3.4.0 | MIT | compiled into `totk.exe` |
| Ryujinx shader translator | 1.3.2 | MIT | `build\xlate\totk_xlate.exe`, a separate process |
| GCC runtime (`libstdc++`) | 16.2.0 | GPL 3.0 with the GCC Runtime Library Exception | linked into `totk.exe` |
| winpthreads (mingw-w64) | 14.0.0 | MIT / BSD | linked into `totk.exe` |

Unicorn (GPL 2.0) stays a separate test tool for the lifter. hactool (ISC) and Capstone (BSD) run at build time.
