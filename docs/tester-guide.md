# Tester guide: running TotK Recomp on your GPU

This guide is for testing the build on a GPU other than the developer's (AMD RX 9070). You build it from source with
your own copy of the game and your own console keys. Nothing from the game is in this repository, and nothing you build
from it may be shared.

## What you need

- Windows 10 or later, an x86-64 CPU with AVX2, about 60 GB free.
- A GPU with Vulkan 1.3 and `VK_EXT_external_memory_host`, with a current driver.
- *Tears of the Kingdom* base game (1.0.0) and the 1.2.1 update, dumped from your own Switch as NSP or XCI.
- `prod.keys` dumped from your own Switch. Never send it to anyone, and never paste it into a log or an issue.

## Build

1. Follow [building.md](building.md): the toolchain, then build steps 1-6 (Python environments, `config.local.toml`
   with your own paths, extraction, the shader translator, lifting and linking, shader preparation). The result is
   `build\v121\totk.exe`.
2. Check the build without the game: `build\v121\totk.exe --selftest` must print a pass.
3. To run the build on another of your machines, package a portable folder ([building.md](building.md), "Portable
   folder"). The folder holds game code: for your own machines only.
4. The installer ([installer.md](installer.md)) is the other route to a machine without the toolchain: it builds the
   game there from your own files. No installer release is published yet.

## Run and collect the log

The log goes to stderr. Run from `cmd.exe` in the repository root, so the log lands in a file:

```bat
set TOTK_VK_VALIDATION=1
build\v121\totk.exe 2> totk-log.txt
```

`TOTK_VK_VALIDATION=1` turns on the Vulkan validation layer (it comes with the Vulkan SDK). It makes the game slower;
do one run with it and one without (`set TOTK_VK_VALIDATION=`).

Keep the window in the foreground: an unfocused window stalls the game. Shaders the preparation did not cover compile on first use, so
the first minutes of play can stutter.
Play into the open field (load a save, walk around for a minute, fight something if you can), then close the window.

## Choosing the GPU

A laptop with an iGPU and a discrete GPU, or two cards, lists every GPU at the top of the log (`vk: gpu 0: ...`).

- `set TOTK_GPU_NAME=Radeon` picks the first usable GPU whose name contains that text (case does not matter).
- `set TOTK_GPU_INDEX=1` picks GPU 1 from that list.

The line `vk: using gpu N: <name>` says which one ran.

## What to send back

1. `totk-log.txt` from each run. It contains no keys and no game files. The lines that matter most start with
   `vk caps:` (GPU, driver, features and the fallbacks taken), `vk: gpu`, `validation` and `gfx_shader:`.
2. GPU model, driver version, CPU, RAM, and whether it is a laptop.
3. What you saw: how far it got (menu, loading, field), roughly how smooth it ran, and screenshots of anything wrong
   (black or missing areas, flickering, wrong colours, missing grass). Win+Shift+S is fine.
4. If it crashed or hung: the last 50 lines of the log and what you were doing.
5. For a bug you can trigger again: a recording of your input (`set TOTK_INPUT_RECORD=input.txt`, see
   [controls.md](controls.md), "Scripted input") and the save you started from.

## Switches for the developer's questions

Only set these when asked; each forces a fallback the build picks by itself on GPUs that need it.

| Variable | Effect |
|---|---|
| `TOTK_VK_NO_FLOAT64=1` | Translate shaders without 64-bit floats |
| `TOTK_VK_SSBO_ALIGN=64` | Treat storage buffers as needing 64-byte alignment |
| `TOTK_VK_SUBGROUP_PIN=0` | Do not pin shaders to 32 lanes; tell the translator the GPU's own subgroup size |

Each switch makes the first launch translate every shader again.
