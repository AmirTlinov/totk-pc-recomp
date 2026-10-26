# Windows toolchain

Native Windows tools, installed with winget unless noted. MSYS2 packages and Python environments are in [toolchain-msys2-python.md](toolchain-msys2-python.md).

Versions are the ones verified on 2026-10-20. Newer patch releases are fine unless the row says otherwise.

| Tool | Version | Install | Verify |
|---|---|---|---|
| Visual Studio Build Tools 2026 (C++ workload, Windows SDK, VC.Llvm.Clang) | MSVC 19.51, SDK 10.0.26100 | `Microsoft.VisualStudio.BuildTools` with `--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.VC.Llvm.Clang` | `cmd /c "call <VS>\VC\Auxiliary\Build\vcvars64.bat && cl"` |
| LLVM | 23.1.3 | `LLVM.LLVM` | `clang --version`, `lld-link --version`, `llvm-objdump --version` |
| CMake | 4.4.4 | `Kitware.CMake` | `cmake --version` |
| Ninja | 1.13.2 | `Ninja-build.Ninja` | `ninja --version` |
| Vulkan SDK | 1.4.363.0 | `KhronosGroup.VulkanSDK` | `glslc --version`, `spirv-cross --revision`, `vulkaninfo --summary` |
| .NET SDK | 10.0.402 | `Microsoft.DotNet.SDK.10` | `dotnet --list-sdks` |
| Rust | 1.99.1, target `x86_64-pc-windows-msvc` | `Rustlang.Rustup`, then `rustup default stable` | `rustc --version` |
| Zig | 0.17.0 | `zig.zig` | `zig version` |
| JDK | Temurin 21.0.12 | `EclipseAdoptium.Temurin.21.JDK` (sets `JAVA_HOME`) | `java -version` |
| Ghidra | 12.1.2 (pinned: SwitchLoader targets it; REA accepts 12.1.x) | Release zip from github.com/NationalSecurityAgency/ghidra into `toolchain/ghidra/` | `toolchain\ghidra\ghidra_12.1.2_PUBLIC\support\analyzeHeadless.bat` |
| Ghidra SwitchLoader | 1.6.1-c600d05 for Ghidra 12.1.2 | Release zip from github.com/Adubbz/Ghidra-Switch-Loader, extracted into `<ghidra>\Ghidra\Extensions\SwitchLoader` | Ghidra lists "Nintendo Switch Binary" as a loader |
| RenderDoc | 1.46 (`e4bd23b`) | Signed MSI from renderdoc.org, unpacked without installing: `msiexec /a <msi> /qn TARGETDIR=<tmp>`, then move `<tmp>\PFiles\RenderDoc\*` to `toolchain/renderdoc/` (drop `System`/`System64`, the VC++ runtime Windows already has). For Vulkan captures, once, as administrator: `renderdoccmd vulkanlayer --register --system` | `renderdoccmd version`; `renderdoccmd vulkanlayer --explain` reports the layer registered |
| REA | rea-agents 6.7.0 | Skill pinned in `.claude/skills/skills-lock.json`; MCP server registered per clone, see [AGENTS.md](../AGENTS.md) | `claude mcp get rea`; `npx -y rea-agents@6.7.0 doctor --json` shows the Ghidra provider available |

## PATH

The User PATH needs these, appended:

- `C:\Program Files (x86)\Microsoft Visual Studio\Installer`: `vswhere.exe`. Without it .NET NativeAOT publish fails at the link step (MSB3073).
- `C:\Program Files\LLVM\bin`: the LLVM installer does not add it.

A 32-bit winlibs MinGW on PATH can shadow `ninja`. Builds call Ninja from `C:\msys64\ucrt64\bin` by full path.

## Smoke tests

- **NativeAOT** (needed for the Ryujinx shader translator): `dotnet new console` then `dotnet publish -c Release -r win-x64 -p:PublishAot=true` produces a native exe that runs.
- **Generated-C dialect**: a C file using `__attribute__((musttail))`, `unsigned __int128` and `vector_size` types builds with MSYS2 UCRT64 `clang -O1 -fno-strict-aliasing -march=x86-64-v3 -fuse-ld=lld` and runs.
- **GPU**: `vulkaninfo --summary` lists the discrete GPU with Vulkan 1.3 or newer.

## Building the game code

`tools/build_game.py` compiles `generated/<version>/` (written by `recomp/gen.py`) and links it with the runtime into `build/<version>/totk.exe`:

```powershell
.venv\Scripts\python tools\build_game.py                    # generated/v121 -> build/v121/totk.exe
.venv\Scripts\python tools\build_game.py --modules subsdk0  # one module; --compile-only skips the link
build\v121\totk.exe --selftest                              # layout, guest window, dispatch table
```

- It writes `build/<version>/build.ninja` and runs `C:\msys64\ucrt64\bin\ninja.exe` with `C:\msys64\ucrt64\bin` first on `PATH`. Unchanged chunks are not recompiled.
- Compile jobs default to the smaller of the CPU count and a 50 GiB memory budget (80% of RAM on smaller machines) at `JOB_MEM` per job. `--jobs N` overrides it.
- Each phase prints its wall time and the peak committed memory of one process and of all processes together, then the exe's section sizes.
- 1.2.1 on 24 threads / 64 GB (2026-10-20): 351 files, 900 MB of C; clean compile 190 s at 24 jobs, under 600 MiB per job and 3.7 GiB all jobs together; link 1 s, 820 MiB working set; `totk.exe` 316 MB with 276 MiB of `.text`, well inside the 2 GiB reach of RIP-relative code.
