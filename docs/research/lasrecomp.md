# LASRecomp study (reference for the TotK port)

Source: `refs/LASRecomp`, commit `da35b83` (2026-10-19, single squashed commit), version `beta_4`
(`runtime/ui/launcher.mm:88`). Paths below are relative to `refs/LASRecomp`.

TL;DR: a clean, small static recompiler (~2.8k lines of Python), a runtime that recompiles on the player's
machine, and an NVN-at-API-level HLE renderer for Vulkan. The CPU pipeline and the Windows platform layer can be
reused with targeted fixes. The kernel, services, NVN HLE, Maxwell shader translator and Vulkan backend cover only
what Link's Awakening calls, so for TotK they need large extensions or replacement.

## 0. Size

| Area | Lines | Reuse for TotK |
|---|---|---|
| Recompiler (`tools/gen.py lifter.py simd_c.py nso.py analyze.py sym.py`) | 2,779 | yes, with fixes |
| Recompiler tests (`tools/test_lift.py test_simd.py` + harnesses) | 865 | yes |
| Runtime core (cpu/dispatch/kernel/ipc/loader/simd/platform/main/paths/settings/romfs/gamelib/gamebuild) | 6,338 | mostly |
| Services (`runtime/services/*.c`) | 2,452 | partly |
| Generic HLE (input, bindings, movie, nvn_boot, pl, swkbd, vi, heaps, framepace) | 2,024 | partly |
| NVN core (`runtime/nvn/objects.c cmdbuf.c tegra.c formats.c astc.c output_scale.c` + headers) | 2,319 | base only |
| Maxwell translator (`runtime/nvn/maxwell/`) | 2,268 | base only |
| Vulkan backend (`runtime/nvn/vulkan/`) | 6,019 | base only |
| Metal backend (`runtime/nvn/*.m`) | 3,892 | no (Windows target) |
| LA-specific: `tools/patches.py`, `warp_table.py` | 958 | no |
| LA-specific HLE (`runtime/hle/{chamber,digs,doorjump,escape,flags_ext,hookshot,keysanity,miniboss,mirror,pets,pot_glow,qol,rescue,shopsanity,souls,warp,widescreen}.c`, `runtime/rando.c`) | 4,593 | no |
| `runtime/nvn/texpatch.c` (LA button-prompt texture edits) | 1,043 | no |
| UI (`runtime/ui/*`, launcher + randomizer UI + overlays) | 6,479 | launcher concept only |
| Randomizer tools (`tools/rando/*.py`) | 8,489 | no |

## 1. CPU pipeline

### Loading
- `tools/nso.py:12-41`: NSO0 header, 3 segments LZ4-decompressed (`lz4.block`), flat image, MOD0 → dynamic section,
  dynsym via `DT_HASH` nchain, RELA + JMPREL relocations.
- Runtime loader `runtime/loader.c`: its own LZ4 decoder (`:17-38`), loads into the guest window at fixed bases,
  applies `R_RELATIVE/ABS64/GLOB_DAT/JUMP_SLOT` (`:143-173`). Imports resolve in this order: host HLE implementation
  by symbol name, then a guest export, then an "unimplemented" stub at a fake address (`:131-141`).
- Only two modules: `main` @ `0x7000000000`, `sdk` @ `0x7004000000` (`runtime/loader.c:177`, `runtime/rt.h:73-74`,
  also hard-coded in `Makefile:191-192`, `runtime/gamebuild.c:304`, `tools/sym.py:15`). `rtld` and `subsdk*` are
  ignored. rtld's job (module init order, `nn::init::Start` arguments) is done by the host (`runtime/main.c:16-36`).
  **The main image (text, rodata, data and bss) must fit in 64 MB**, or `SDK_BASE` has to move.

### Decoding
- **Capstone is not used by the lifter.** `tools/lifter.py` is a hand-written AArch64 decoder (op0 split at
  `:180-194`). Capstone is used only in `analyze.py` (instruction census), `sym.py` (disassembly for debugging) and the
  tests.
- Unsupported encodings raise `Unsupported` and become `rt_udf(c, pc)` traps in the output (`tools/gen.py:60-69`), so a
  build never fails on an unknown instruction; it crashes at run time if the instruction executes.
- Gaps that matter for TotK:
  - Exclusive pair `LDXP/STXP/LDAXP/STLXP` (`o1=1`) raises Unsupported (`lifter.py:391-392`). 128-bit lock-free code
    (tagged pointers) will trap.
  - Register-offset atomics (LSE) are not supported (`lifter.py:445-446`). That is fine: the Switch's Cortex-A57 is
    ARMv8.0.
  - `simd_c.py:1-6`: ARMv8.0 + Crypto + CRC32 only. FP16 arithmetic, RDM, dot product, FCMA and JSCVT are rejected
    (not present on A57).

### Function, CFG and jump-table discovery (`tools/gen.py`)
- Seeds (`boundaries`, `:71-83`): `.eh_frame_hdr` table (`analyze.py:28-46`), exported FUNC symbols, and targets of
  `R_AARCH64_RELATIVE` relocations that land in `.text` (vtables, callbacks).
- More seeds (`:519-546`): every `BL` target, `ADR` targets, and `ADRP`+`ADD` pairs within 8 instructions.
- Fixpoint (`:547-561`): a function is the contiguous range `[start, next_start)`. `scan()` (`:298-328`) collects
  branches. A branch target outside the range becomes a new start. Repeat until no new starts appear. Results are
  cached per `(start, end)`.
- Jump tables (`:98-277`): **Unicorn** single-steps the code between the bounds check (`cmp #N; b.hi/b.ls`) and the
  `br`, once for each index `0..N-1` (at most 4096). It first replays the `ADRP/ADR/ADD` instructions from the
  function entry so the table base is known.
  - Unresolved computed `br`: `tables[a]='all'` (`:319-322`).
  - Any function with a table gets a label on every instruction and an `L_dispatch` switch over all of its instruction
    addresses (`:378-382`, `:459-463`). This is the fallback when emulation missed cases.
- Indirect `br` from a load (tail call through a pointer) → `rt_dispatch`.

### Emitted C
One C function per guest function, `void f_<addr>(Cpu *c)`. Templates are in `gen.py:355-466` and `lifter.py`.

Representative shape, assembled from those templates:

```c
#include "cpu.h"
#include "simd.h"                      /* --simd=c (x86 hosts) */
void f_7000123460(Cpu *c);
void f_7000123400(Cpu *c) {
    FN_ENTER(0x7000123400ull);                                   /* profiler hook, one predictable branch */
    { uint64_t ea = c->sp - 32; ST64(ea, c->x[29]); ST64(ea + 8, c->x[30]); c->sp = ea; }   /* stp pre-index */
    c->x[29] = (uint64_t)(c->sp + 0x0ull);                       /* mov x29, sp */
    (void)(subs64(c, c->x[0], 0x10ull));                         /* cmp x0, #16 (flags into c->nzcv) */
    if (COND_HI(c)) { goto L_7000123440; }
    c->x[30] = 0x7000123418ull; f_7000123460(c);                 /* bl: direct host call */
    c->pc = c->x[8]; c->x[30] = 0x7000123420ull; rt_dispatch(c); /* blr x8 */
    SV_FOP_S(FOP_ADD, 1, 0, VREG(vf32, 1), VREG(vf32, 2), (vf32){});  /* fadd v0.4s, v1.4s, v2.4s (simd_c.py:437-469) */
    switch (c->x[9]) { case 0x7000123430ull: goto L_7000123430; /*...*/ default: break; }
    c->pc = c->x[9]; goto L_dispatch;                            /* br x9 through a resolved jump table */
L_7000123440:;
    { uint64_t ea = c->sp; uint64_t a = LD64(ea), b = LD64(ea + 8); c->sp = ea + 32;
      c->x[29] = (uint64_t)(a); c->x[30] = (uint64_t)(b); }       /* ldp post-index */
    return;                                                      /* ret */
L_dispatch:
    switch (c->pc) { /* every instruction address of this function */ default: break; }
    __attribute__((musttail)) return rt_dispatch(c);
}
```

- `b` to another function: `__attribute__((musttail)) return f_X(c);` (`gen.py:370-376`).
- `ret` is a plain C `return;`. The model assumes strict call/return discipline: `x30` is never checked. Guest
  setjmp/longjmp, fibers, or code that changes LR before `ret` would break. LA needs none of these (no handling exists
  in the runtime).
- Patch hooks:
  - `PATCHES[addr]` replaces single instructions (`gen.py:408-412`, `tools/patches.py`).
  - `RANDO_CODE` emits seed-selected instruction variants (`gen.py:397-407`).
- Output is chunked at ≤1500 functions or ~1.5 MB of C per file. A function with >100 KB of C gets its own file.
  Unchanged chunks are not rewritten, so `make` stays incremental (`gen.py:480-490`, `:499-501`, `:577-608`).
- `<mod>_table.c` holds sorted `fn_addrs[]` and `fn_ptrs[]` (`gen.py:613-625`).

### Indirect calls and branches (`runtime/dispatch.c`)
- `rt_dispatch` (`:160-164`):
  1. Per-thread direct-mapped cache, 4096 entries keyed by `pc>>2`.
  2. On a miss, `dispatch_slow` (`:134-157`): HLE fake-address range, then a linear scan of overrides, then a binary
     search of the two sorted tables (`:115-128`).
  3. Always `musttail`, so chains of indirect jumps don't grow the host stack.
- HLE functions live at fake addresses `HLE_BASE + 16*i`, at most 4096 of them (`:14-57`).
- Unknown address: `FATAL`.
- `main` → `sdk` calls go through PLT/GOT slots (relocated function pointers), so every SDK call from the game is an
  indirect dispatch.

### FP/SIMD
- On arm64 hosts: raw `.inst` passthrough in inline asm, V regs loaded from and stored to `Cpu` around each batch of up
  to 24 instructions (`lifter.py:680-728`).
- On x86 (Windows): `--simd=c` (`gen.py:503-504`). `simd_c.py` decodes each encoding at lift time into calls to
  `always_inline` helpers in `runtime/simd.h` and `simd_fp.h` with constant fields, so clang folds the switches.
- Semantics follow the ARM pseudocode (`simd.h:1-8`): NaN propagation order, FPCR.DN/FZ/RMode, saturating FP→int,
  fused FMA, ARM estimate tables. Not modelled: FPSR cumulative bits and QC, trap enables.
- Requires x86-64-v3 (AVX2, FMA, BMI2). Checked at startup in `runtime/platform/windows.c:143-149`.
- FPCR/FPSR live in `Cpu` (`cpu.h:17-18`). All V regs live in `c->v[32]` in memory (`cpu.h:20`).
- Tested by differential fuzzing against Unicorn. `tools/test_simd.py` enumerates the whole FP/SIMD encoding space;
  `tools/test_lift.py` covers the integer path one instruction at a time.

### Exclusive monitor and atomics (`runtime/cpu.h:73-97`)
- `ldxN` records `(addr, value)` in `Cpu`. `stxN` does a CAS against the recorded value: ABA-tolerant, standard
  practice.
- `LDAR/STLR` → `__atomic` acquire/release.
- `CLREX` → `excl_addr = ~0` (`lifter.py:325-326`).
- `DMB/DSB` → `__atomic_thread_fence(SEQ_CST)` (`lifter.py:327`).
- `DC ZVA` → 64-byte `memset` (`:330-331`). Other cache maintenance is a no-op.

### System registers, TLS, timing
- `TPIDRRO_EL0` = `c->tpidrro`, a per-thread 0x200-byte TLR in `TLS_BASE` (`runtime/kernel.c:201-221`).
- `TPIDR_EL0` is user-writable (`lifter.py:337, 351`). musl TLS is handled by the guest SDK itself.
- `CNTFRQ` = 19.2 MHz. `CNTVCT/CNTPCT` → `rt_ticks()`.
- `DCZID` = 4. `CTR_EL0` = `0x8444c004` (`lifter.py:335-346`).

### Memory model (`runtime/rt.h:68-84`, `runtime/platform/windows.c:13-82`)
- **Identity mapping: guest address == host address.** `LD64(a)` is `*(u64u*)(uintptr_t)a` (`cpu.h:53-71`). There is
  no base register and no offset.
- Window `0x70_0000_0000 .. 0x73_2000_0000` (12.5 GB):
  - modules
  - `HLE_BASE 0x7080000000`
  - `TLS_BASE 0x7090000000`
  - `HLE_HEAP 0x7092000000` (224 MB)
  - `ALIAS 0x7100000000` (4 GB)
  - `STACK 0x7200000000` (1 GB, `kernel.h:7-8`)
  - `HEAP 0x7240000000` (3.25 GB max)
- Windows: the window is reserved by `VirtualAlloc(MEM_RESERVE | MEM_WRITE_WATCH)` in a priority-101 constructor
  (`runtime/main.c:37-52`). If the range is taken, the process re-executes itself, up to 20 times.
- Regions are committed on map, and a vectored exception handler lazily commits 64 KB on the first stray access
  (`windows.c:21-38`).
- `MEM_WRITE_WATCH` feeds the Vulkan vertex/index cache (`windows.c:40-63`).
- `svcMapMemory` copies the data instead of aliasing it (`kernel.c:580-589`).
- No page protection emulation, no MMIO.

### Stack and threads
- Guest stack: in guest memory (`c->sp`).
- Host stack: every guest `BL` is a host C call, so guest recursion runs on the host stack. Each guest thread gets a
  256 MB host stack (`kernel.c:226`), reserved rather than committed on Windows (README credit #107).
- One guest thread = one pthread (winpthreads on Windows), started by `thread_main` (`kernel.c:187-199`).
- Priorities are stored, core masks are ignored (`kernel.c:642`). There is no scheduler.
- One global `g_klock` guards all kernel state. Each thread has its own cond var.
- In-place reboot: `longjmp` to `thread_main` at safe points (`kernel.c:173-185`).

## 2. Runtime / HLE

### Kernel (`runtime/kernel.c`, 868 lines)

| SVC | Behaviour |
|---|---|
| 0x01 | SetHeapSize |
| 0x02, 0x03 | No-op OK |
| 0x04, 0x05 | Map/Unmap by copy |
| 0x06 | QueryMemory |
| 0x07 | ExitProcess |
| 0x08–0x10 | Threads |
| 0x11–0x17 | Events, shared memory, transfer memory |
| 0x18, 0x19 | WaitSynchronization, Cancel |
| 0x1A–0x1D | ArbitrateLock/Unlock, CondVar |
| 0x1E | GetSystemTick |
| 0x1F | ConnectToNamedPort (only `sm:`) |
| 0x21, 0x22 | SendSyncRequest (with user buffer) |
| 0x24–0x27 | |
| 0x29 | GetInfo (fixed values) |
| 0x2C, 0x2D | Map/UnmapPhysicalMemory |
| 0x34, 0x35 | WaitForAddress, SignalToAddress |
| 0x45 | CreateEvent |
| anything else | `FATAL` (`:785`) |

- Handle table: 4096 handles (`kernel.c:30`).

### IPC (`runtime/ipc.c`, 246 lines, plus `ipc.h`)
- HIPC/CMIF parsing and serialization.
- Services register through a linker-section table, `SERVICE("name", handler)`. On PE/COFF it uses the
  `.svc$a/$m/$z` trick (`ipc.c:8-17`).
- An unknown service gets a logging stub that returns OK with 0x40 zero bytes (`ipc.c:19-24, 34-41`).
- Implemented services:

| Service | Notes |
|---|---|
| `sm:` | |
| `acc:u0` | |
| `appletOE` | no applets (`swkbd.c` HLEs the keyboard) |
| `apm` | |
| `lm` | |
| `audren:u` | CPU mixer modelled on Ryujinx's AudioRenderer: PCM16/ADPCM, resampling, sub-mixes → SDL3 audio stream. Effects and splitters not implemented (`audio.c:14-18, 875`) |
| `audout:u` | |
| `fsp-srv` | RomFS as a raw image plus overlays; save data and SD card mapped to host dirs. `romfs_build.c` builds RomFS images |
| `hid`, `hid:sys` | shared memory filled from SDL3 keyboard/gamepad (`runtime/hle/input.c`, `bindings.c`) |
| `set`, `set:sys` | |
| `pctl*` | |
| `aoc:u` | |
| `time:u/a/s` | |

### Function-level HLE
`HLE_FUNC("mangled", fn)` binds guest imports by name (`runtime/hle.h:150-166`):
- `vi`/`nv` display init (`hle/vi.c`)
- `nn::pl` shared fonts
- `movie::` (nnMedia) on FFmpeg (`hle/movie.c`, 796 lines)
- swkbd
- heap wrappers
- `nvnBootstrapLoader` / `nvnDeviceGetProcAddress` (`hle/nvn_boot.c`)

`hle_override(addr, fn)` replaces functions by address, but only on the dispatch path (indirect calls).
`runtime/hle/hle_symbols.txt` is empty.

### Recompile on the player's machine (`runtime/gamebuild.c`, `runtime/gamelib.c`)
- The release contains no game code.
- On first run, the start screen does three things:
  1. Runs the PyInstaller-frozen `gen.py` (`tools/lasgen.spec`).
  2. Compiles the chunks with bundled `zig cc` as they appear. Jobs = `min(cores, RAM_GB-2)`
     (`runtime/ui/launcher.mm:283-285`): budget about 1 GB RAM per compile job.
  3. Links `lasgame.dll` with `zig lld-link`.
- The DLL exports only `lasgame_tables()` (`tools/gamelib_entry.c`).
- It calls back into the exe through an import library. The exe is linked with `--export-all-symbols --out-implib`
  (`Makefile:104-106`).
- `GAMELIB_ID` (a hash of the generator, the headers and the flags) forces a rebuild when they change
  (`Makefile:111-114`).
- The UI says the build takes "a few minutes" for LA (`launcher.mm:282`).

## 3. Graphics

### NVN on Windows
- NVN is HLE'd at the **API level**, not the GPU command level. Every `nvn*` proc address resolves to a host stub
  (`hle/nvn_boot.c`). About 495 `NVN(...)` entry points exist (`objects.c` 371, `cmdbuf.c` 124).
- Command buffers are recorded as `CmdRec` records into guest command memory (`cmdbuf.c:1-3`), then executed by the
  Vulkan backend (`runtime/nvn/vulkan/exec.c`, 3.8k lines).
- Vulkan 1.3 is required (dynamic rendering: `VkPipelineRenderingCreateInfo`, `exec.c:1513`). Allocation uses VMA.
- Missing NVN entry points silently `return 0` (`dispatch.c:69-75`).

### Shaders
- Own Maxwell SM5.x decoder in `runtime/nvn/maxwell/translate.c` (2.2k lines). Pipeline: emit MSL, token-rewrite to
  GLSL 4.60, compile with shaderc (runtime, Apache-2.0) to SPIR-V (`translate.c:1-12`, `shader.h:1-27`).
- Branches become a `switch(pc)` state machine with explicit SSY/PBK/PCNT stacks.
- Handled opcodes: BAR BFE BFI BRA BRK BRX CONT CSETP EXIT F2F F2I FADD FADD32I FCMP FFMA FFMA32I FLO FMNMX FMUL
  FMUL32I FSET FSETP FSWZADD I2F I2I IADD IADD3 IADD32I ICMP IMNMX IMUL IMUL32I IPA ISCADD ISCADD32I ISET ISETP KIL
  LD(attr) LDC LDG/STG LDL/STL LOP LOP3 LOP32I MOV MOV32I MUFU P2R PBK PCNT POPC PRMT PSET PSETP R2P S2R SEL SHF SHFL
  SHL SHR SSY ST(attr) SYNC TEX TEXS TLDS TXQ XMAD.
- **Missing** (`fail()` at `translate.c:1624`):
  - shared memory LDS/STS
  - atomics ATOM/ATOMS/RED
  - surface load/store SULD/SUST
  - TLD, TLD4 (gather), TXD, TMML
  - VOTE
  - double precision
  - CSET
  - bindless TEX.B (`translate.c:760`)
- Graphics pipelines are VS+FS only (`stageCount = 2`, `exec.c:1516`, also `:1092, :3269, :3330`), plus compute. No geometry or tessellation.
- Pipeline caching:
  - SPIR-V disk cache `cache/spirv/<hash>.spv` (`vulkan/shaders.c:20-54`).
  - `VkPipelineCache` saved to `cache/vk_pipelines.bin` (`vulkan/device.c:217-248`).
- One translator thread plus N pipeline worker threads (`exec.c:566-640`). Draws are skipped until their pipeline is
  ready (`exec.c:2685`), so new areas show pop-in.

### Textures
- Tegra block-linear deswizzle on the CPU (`runtime/nvn/tegra.c`).
- BCn formats map natively (`vulkan/textures.c:92-104`).
- **ASTC is decoded on the CPU to RGBA8** (`runtime/nvn/astc.c`, LDR 2D only, bit-exact with astcenc;
  `vulkan/textures.c:299-308, 548-555`).

### Other
- Resolution scaling, SMAA and FSR1 options (`output_scale.c`, `smaa_data.c`).
- Write-watch vertex cache (`exec.c:2048-2150`).

## 4. Windows toolchain

### Dev build
- MSYS2 UCRT64 (`tools/setup_windows.ps1`): clang, lld, make, SDL3, FFmpeg, vulkan-devel, shaderc, spirv-tools, VMA,
  validation layers, nasm.
- Python 3.12 from python.org, with venv `tools/venv` (capstone, unicorn, lz4).
- Build commands: `make gen` then `make -j` (`docs/BUILD-windows.md`).

### Flags (`Makefile`)

| What | Flags |
|---|---|
| Generated code | `clang -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime -D_FILE_OFFSET_BITS=64 -march=x86-64-v3` (`:39, :91-92`) |
| Runtime | `-O2 -g -fno-strict-aliasing -Wall ...` (`:117`) |
| Exe link | `clang++ -fuse-ld=lld -Wl,--stack,16777216 ... -lvulkan-1 -lshaderc_shared -Wl,--export-all-symbols -Wl,--out-implib` (`:93-95, :104`) |
| Game DLL link | `clang -shared -fuse-ld=lld build/lasrecomp.dll.a` (`:105`) |

### Player build
- zig 0.17.0 `zig cc` with `-mcpu=x86_64_v3`, linked with `zig lld-link @rt/link.rsp`.
- The zig runtime is prebuilt and the bundle slimmed by `tools/zig_slim.sh`; the exe is fetched by
  `tools/fetch_zig.sh`.
- The `lasgen.exe` PyInstaller bootloader needs Control Flow Guard cleared, because Unicorn longjmps out of its JIT
  (`tools/pe_no_cfg.py`).

### No MSVC dependence
- Target is the mingw-w64 / UCRT ABI.
- The generated C needs GNU/Clang extensions: `__int128`, `vector_size`, `__builtin_shufflevector/convertvector`,
  `musttail`, statement expressions, inline asm on arm64. So `cl.exe` cannot compile it; clang-cl would need checking.

### Unknown figures
- Generated file count, C size and compile time are not recorded in the repo.
- Chunking bounds them at 1.5 MB per file. LA main `.text` is about 16.5–23 MB (code patch at `0x7001071220`, rodata
  literal at `0x70016afba4`, `tools/patches.py`). That is roughly 4–6 M instructions, so LA is not "small".
- Measure TotK with `python tools/nso.py <exefs>/main` before estimating.

## 5. Game-specific vs generic
- LA-specific, around 22k lines in total:
  - `tools/patches.py` (687 lines, 60 hard-coded `0x70…` addresses: widescreen, warp, rando hooks)
  - `tools/warp_table.py`, `tools/rando/*` (8.5k)
  - `runtime/hle/*` LA files (4.6k), `runtime/rando.c`
  - `runtime/nvn/texpatch.c` (1k)
  - most of `runtime/ui/*` (6.5k)
  - `third_party/LAS-Randomizer`
  - hard-coded addresses in `runtime/nvn/cmdbuf.c`, `runtime/hle/movie.c`, `runtime/fnprof.c`
    (`grep -l 'main+0x\|0x70[0-9a-f]{8}' runtime`)
- Generic, around 18k lines: recompiler, cpu/dispatch/loader/kernel/ipc, services, platform layer, NVN core, Maxwell
  translator, Vulkan backend, ASTC, tegra, generic HLE.
- About 45% of the non-third-party code is LA-only. Of the generic part, the CPU side is reusable almost as-is, while
  the GPU/kernel/service side holds only what LA calls.

## 6. Scaling risks for TotK (concrete)

1. **Python generator memory and time.**
   - `Module.cache` keeps one `Insn` object (with C strings) per instruction (`gen.py:60-69`), and `scanned` keeps
     label sets per function. At 10M+ instructions that is several GB of RAM. Lifting is single-process pure Python.
   - Fix: stream functions, drop caches after emission, parallelize lifting across processes (functions are
     independent once boundaries are fixed).
2. **Jump-table emulation cost.** Each table index replays address materialization from the function entry
   (`gen.py:159-161, 257-264`), so cost is O(entries × function length) per table, through Unicorn single-steps from
   Python. Large switch-heavy functions (state machines, AI scripts, Havok) will be slow.
3. **"All labels" fallback** (`gen.py:378-382, 459-463`). Any function with a jump table gets a label on every
   instruction and a switch over all of them. Clang -O1 compile time and RAM grow badly, and register promotion dies.
   This drives the ~1 GB per compile job budget. Needs a precise table resolver (data-flow on index register and table
   base, no emulation) so the fallback is the exception.
4. **Codegen quality.**
   - Guest regs live in `Cpu` in memory. `LD/ST` use `may_alias` types plus `-fno-strict-aliasing` (`cpu.h:53-71`,
     `Makefile:39`), so clang must assume every guest store can clobber `c->x[]`/`c->v[]` and reload them.
   - SIMD helpers carry NaN/FZ checks per op.
   - Built at `-O1`.
   - Fine for LA at 60 fps; TotK is CPU-bound on Switch.
   - Options: cache guest regs in C locals with explicit spill at calls and dispatch; put `Cpu` behind
     `restrict`/noalias; FPCR-specialized fast paths.
5. **Fixed layout.** `SDK_BASE = MAIN_BASE + 64 MB` (`rt.h:73-74`, `Makefile`, `gamebuild.c:304`), only 2 modules
   (`loader.c:177`), `g_modules[4]`. Make it data-driven (N modules, bases computed from sizes).
6. **Dispatch.**
   - 4096-entry per-thread direct-mapped cache; binary search on a miss; linear `overrides` scan on every slow path
     (`dispatch.c:145-146`).
   - TotK's virtual-heavy code and many threads will miss more.
   - Better: a perfect hash or a dense table indexed by `(addr - text_lo) >> 2` (4 bytes × text/4 ≈ text-size
     memory).
7. **Missing ISA.** `LDXP/STXP` (`lifter.py:391-392`) trap at run time. Add them via `cmpxchg16b` (x86-64-v3 has it).
   Also log a census of `Unsupported` per build (`gen.py:66`).
8. **Call model.** `ret` = C return; guest `BL` = host call. Guest setjmp/longjmp, `nn::os` fibers, or exception
   unwinding would break. Audit TotK's SDK imports for `setjmp`, `longjmp`, `__cxa_throw`, `nn::os::Fiber`.
9. **Kernel.**
   - One global lock; no scheduler; core masks ignored.
   - Unimplemented SVCs are FATAL. The SVC list is LA-scoped: no MapProcessMemory, CreateCodeMemory,
     SetThreadActivity, GetThreadContext, etc.
   - Handle cap 4096.
   - TotK has many worker threads (job system); expect contention on `g_klock`.
10. **GPU.**
    - Translator lacks shared memory, atomics, image load/store, gathers, TXD/TMML, bindless and VOTE, and has no
      geometry/tess stages. TotK compute and deferred passes will hit these.
    - NVN coverage is only what LA calls, and missing calls silently return 0.
    - CPU ASTC decode plus RGBA8 upload is 4× VRAM and a large CPU cost on a heavily ASTC game. Use a GPU compute
      ASTC decoder.
    - Draw skipping on pipeline misses means visible pop-in.
11. **Audio.** No effects or splitters (`audio.c:875`), so TotK reverb/delay will be missing.
12. **Toolchain cost.** Chunk compile at ~1 GB per job. 64-job cap. Single lld link of one DLL. Expect link RAM and
    time to scale with object size; consider splitting the game DLL or using LTO-free thin archives.

## 7. Licensing

| Component | License | Where | Impact |
|---|---|---|---|
| LASRecomp code | MIT (`LICENSE`, © 2026 Peiikai) | all of `tools/`, `runtime/` | Reusable with attribution |
| Capstone 5.0.9 | BSD-3 | analyze/sym/tests only | Fine |
| Unicorn 2.1.4 | **GPL-2.0** (Python bindings BSD) | `gen.py` jump tables, tests | Build-time helper only (separate process). Shipping a frozen generator that bundles Unicorn makes that helper a GPL distribution. Replacing it with an own resolver removes the issue |
| Keystone 0.9.2 | **GPL-2.0** | `tools/rando/collect_patches.py` only | Not needed for TotK |
| oead / evfl | **GPL-2.0+** | randomizer only | Not needed |
| LAS-Randomizer | MIT | randomizer | Not needed |
| python-lz4 / LZ4 | BSD | nso.py | Fine |
| Dear ImGui, VMA, SMAA, AMD FSR1 | MIT | UI, renderer | Fine |
| SDL3 | zlib | window/input/audio | Fine |
| FFmpeg 9.0.2 | LGPL-2.1+ (release built LGPL-only, `tools/build_deps_windows.sh`) | movies | Dynamic link only |
| shaderc | Apache-2.0 | GLSL→SPIR-V | Fine |
| zig 0.17.0 / LLVM | MIT / Apache-2.0 w/ LLVM exception | player-side compiler | Fine |
| PyInstaller | GPL-2.0+ w/ bootloader exception | freezing helpers | Fine |
| GCC runtime, winpthreads | GPL-3 w/ runtime exception, MIT/BSD | Windows DLLs | Fine |
| Baloo 2 font | OFL-1.1 | start screen | Fine |
| Ryujinx (audio design reference, `audio.c:14-18`) | MIT | design only | Fine |

envytools docs were used for the shader translator. For the TotK shader work, the Ryujinx shader translator (MIT, C#)
is a license-compatible reference. yuzu's `shader_recompiler` is GPL-2.0+, so code cannot be copied from it into an MIT
codebase.

## 8. Recommendation for the TotK port

### Reuse with fixes
- `nso.py`
- `lifter.py` (add LDXP/STXP)
- `simd_c.py` + `simd.h` + `simd_fp.h` (tested; add fast paths later)
- `cpu.h`, `dispatch.c` (replace the lookup structure), `loader.c` (N modules)
- `platform/windows.c` (reserve + lazy commit + write watch is directly applicable)
- `ipc.c` framework, `services/fs.c` + `romfs_build.c`, `services/time.c`, `hid_set.c`, `hle/input.c`
- `nvn/tegra.c`, `nvn/astc.c` (as the reference/CPU fallback)
- Test harnesses `test_lift.py`, `test_simd.py`, `test_translate.*`, `test_astc.*`
- The recompile-on-user-machine distribution model (`gamebuild.c`, `zig_slim.sh`)

### Rewrite or replace
- `gen.py` driver: scalable, parallel, streaming, precise jump tables without Unicorn
- Register caching in codegen
- Kernel scheduler and locking
- Most of the NVN HLE and Vulkan backend
- Maxwell translator: needs a full SM5.x IR with compute/atomics/images/bindless/gathers
- GPU ASTC
- Audio effects

### Drop
- `patches.py` contents, rando, LA HLE, LA UI, Metal backend
