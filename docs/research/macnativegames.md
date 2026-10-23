# MacNativeGames / d2r-mobile / nxnative: research notes

Scope: what the three reference ports do, what transfers to an x86-64 Windows port of TotK, what is Apple-only.
Checkouts (read-only, HEAD at time of study, 2026-10-20):

| Repo | Path | HEAD |
|---|---|---|
| MacNativeGames | `refs/MacNativeGames` | `b75be8c` |
| d2r-mobile | `refs/d2r-mobile` | `53e7c5b` (MacNativeGames copied its generic parts from `9f0c257`) |
| nxnative | `refs/nxnative` | `a35e964` |

All paths below are relative to `refs/MacNativeGames` unless prefixed.

## 0. Architecture in one paragraph

Not an emulator and not a recompiler. The game's three ARM64 NSOs (`main`, `sdk`, `subsdk0`; `rtld` is dropped and the
host links) are rewritten offline so they can run natively on Apple ARM64 (`games/totk/third_party/d2r/patch_nso.py`).
A C host maps them at original relative layout, resolves imports itself, and replaces Horizon at two levels:
(a) kernel SVCs via a "gate" trampoline, (b) whole `nn::*` SDK functions overridden by mangled name at relocation time.
NVN is replaced entirely: `nvnBootstrapLoader` is overridden, all 523 NVN entry points route to a host implementation
that records commands into game memory and replays them on Metal 4. Shaders: Maxwell code -> Ryujinx translator ->
SPIR-V -> SPIRV-Cross -> MSL -> Metal, cached on disk, partially pre-built at app build time.

Consequence for us: the CPU side of their work (rewriter, gate) is ARM64-host-only and useless on x86-64 as code, but it
is an exact, verified census of every place game code touches the OS. Everything above the CPU boundary (SVC
semantics, `nn::*` overrides, NVN command model, shader pipeline, TotK patches, data formats) is reusable knowledge, and
much of it is plain C.

## 1. Build pipeline: XCI/NSP + prod.keys -> app

Entry: `build_app.py` (1028 lines). Steps documented at `build_app.py:1-30` and `docs/how-it-works.md:73-125`.

### 1.1 Inputs

- Base game 1.0.0 only (`0100F2C0115B6000`), XCI or NSP. NSZ/XCZ base unsupported. Personalized tickets unsupported.
- Optional cumulative update NSP/NSZ via `--update`: 1.1.2 (v262144), 1.2.1 (v393216), 1.4.3 (v720896). No intermediate
  update needed. DLC and combined packages unsupported (`README.md:66-67, 204`).
- `prod.keys`. NSP needs `--firmware <dir of .nca>` or `--shared-fonts <dir>` (NSP has no system fonts; XCI reads its
  update partition) (`README.md:67, 100`).
- Python deps (`games/totk/requirements.txt`): `cryptography==50.0.2`, `lz4==4.4.5`, `capstone==5.0.9`, `nsz==5.0.0`.
  Python 3.14 is required because the build uses stdlib `compression.zstd` (`requirements.txt:1`,
  `scripts/locate_shaders.py:24`).

### 1.2 Code (step 1, `build_app.py:384-426`)

1. Container detection by header: XCI = HFS0 secure partition, NSP = PFS0 (`docs/how-it-works.md:75`).
2. `games/totk/scripts/extract_exefs.py`: own pure-Python reader, no hactool. NCA3, key-area or common-ticket AES-CTR,
   hierarchical SHA-256 PFS0. Title key from ticket decrypted with titlekek from `prod.keys`, in memory only
   (`extract_exefs.py:1-5, 93-146`). Ticket signature type `0x10004` is little-endian `04 00 01 00`
   (`docs/how-it-works.md:132`).
3. ExeFS hashes compared to `expected-<ver>.json`. A different `main.npdm` is accepted if its ACID signature verifies
   against Nintendo's public ACID modulus (`build_app.py:57-70, 146-171`).
4. `scripts/prepare_modules.py` -> ELF inventory; `third_party/d2r/patch_nso.py <nso> <out> <name>` for sdk, subsdk0,
   main with `DM_RULESET=native DM_PRIVATE_GATES=0`; output hash must equal `rewritten_sha256` (`build_app.py:410-418`).
5. `scripts/prepare_runtime.py` binds the C loader to the verified images: emits `build/runtime_tables.h` (symbols,
   relocations incl. RELR, gates) (`scripts/prepare_runtime.py:1-25`).

### 1.3 Data (step 2, `build_app.py:429-470`, `573-597`)

- Base RomFS: `scripts/extract_romfs.py`, decrypts the 17 GB RomFS into one flat `romfs.bin` + `index.json`, verified
  through all six IVFC levels, then whole-image SHA-256 compared (`build_app.py:433-441`).
- Update merge (BKTR): hactool, pinned `https://github.com/SciresM/hactool.git` @ `1d64a83450e025622f3468c28fc4164dad2c5ef6`
  (`build_app.py:44`). `build_hactool()` patches the fetched source before `make CC=clang` (`build_app.py:473-570`):
  1. `MNG_TITLE_KEYS` env: private title-key file for both tickets, mode 0600, deleted after merge.
  2. `MNG_BASE_RANGE_OFFSET/SIZE`: funopen-backed bounded view of the base NCA inside the original XCI/NSP, avoiding a
     second 17 GB copy (Darwin `funopen`; Windows needs another mechanism or a temp copy).
  3. Two BKTR layout checks relaxed: newer updates leave sector-aligned padding after the subsection table
     (`build_app.py:542-565`).
  Output via `--sectionN` (not `--romfs`, which does not export the BKTR section) (`docs/how-it-works.md:119`).
  Driver: `scripts/extract_update_romfs.py`.
- Update version is chosen by exact ExeFS hashes, not file name; BKTR section marker identifies an update NCA
  (`docs/how-it-works.md:117`).
- Shared fonts: `scripts/extract_shared_fonts.py`. Six BFTTF from system data titles starting `0100000000000811`;
  BFTTF = big-endian words XORed with `key = word0 ^ 0x7f9a0218`, word1 = size (`extract_shared_fonts.py:19-28, 122-128`).
  Other firmware fonts accepted if structurally valid.
- RomFS overlays from the player's files (manifest: path, size, sha256 overlay, sha256 original):
  - `scripts/make_romfs_patches.py`: quick-menu capture timing (`UI/LayoutArchive/Common.Product.100.Nin_NX_NVN.blarc.zs`
    anim keys moved to frame 0) and 1.0.0 game-over retry (`Event/EventFlow/DmF_SY_GameOver.bfevfl.zs`) (header lines 1-25).
  - `scripts/make_output_res_overlay.py`: HeapDef for 2560x1440 (see section 6.3).

### 1.4 Shaders and packaging (steps 3-4)

- SPIRV-Cross @ `aa217aeb6c9f0ace7a0ab233b28807edf45eb165`, compiled from 11 sources with plain clang, no CMake
  (`build_app.py:49-54, 600-612`).
- Prebuilt `games/totk/prebuilt/macos-arm64/totk_xlate` checked against `totk_xlate.sha256` (`build_app.py:615-630`).
- `scripts/prepare_shader_seed.py` extracts every shader from the verified RomFS + main module, translates default-bind
  variants plus every `V` row from `assets/shader_list.tsv`; refuses to ship fewer than 8,000 (`docs/first-launch-shaders.md`).
  1.4.3: 8,290 unique shaders, 24,871 runtime files, ~334 MiB seed.
- Optional: `metal` compiles MSL -> `.metallib` per source hash (`build_app.py:674`); `tools/warm_metal_cache.m`.
- Runtime binary built per version (`games/totk/Makefile:71-72, 341, 351, 361`: `-DTOTK_GAME_V112/V121/V143`), bundled,
  codesigned, `--check` run.

### 1.5 Verification hashes (`games/totk/assets/expected-v*.json`)

| | 1.0.0 | 1.1.2 | 1.2.1 | 1.4.3 |
|---|---|---|---|---|
| `main` sha256 | `faf81f8a…a3eaab` | `f04fbebd…08d1ac` | `e5aa8d48…efcb6` | `e84a90d5…2dc9e` |
| `sdk` | `91573d44…4eea` | same | same | `1d187f29…e4ad` |
| `subsdk0` | `9d04c989…3f99` | same | same | `767c35bc…79b1` |
| `rtld` | `28317605…a44d` | same | same | `49308064…2817` |
| RomFS size | 17,069,139,640 | 17,051,988,992 | 17,052,047,360 | 17,052,625,232 |
| RomFS sha256 | `4751542f…dafcf` | `d98fde08…c43` | `c2beb1c2…7c11` | `797e66f2…19e9b` |

Full hashes in the JSON files. Fonts identical across versions. Base card image 17,536,620,544 bytes, program NCA
`9d827368992a2faafa9062aa5458b7ec.nca` (`expected-v1.0.0.json`). 1.0.0-1.2.1 share sdk/subsdk0/rtld; 1.4.3 replaced
all three (new SDK, RELR relocs, GNU-hash-only SDK modules) (`docs/how-it-works.md:125`). nxnative records 1.4.3 build IDs:
`main=277178b7dba1b6d4949a84778d4abc58 subsdk0=f39f4c431234c72eb520cfdd1001bbcf sdk=574284b40c06cf1821a44857939083be`,
SDK 20.5.22 (`refs/nxnative/dm/games/0100F2C0115B6000/game.tsv`).

These hashes are directly reusable for our own input validation.

## 2. NSO rewriter: every place game code touches the OS

File: `games/totk/third_party/d2r/patch_nso.py` (1036 lines, Python + capstone), plus `call_window.py`, `relr.py`.
Verification: `structck.py` (structural, independent llvm-objdump recount), `equiv.py` (Unicorn per-site differential
with mutation negative controls).

### 2.1 Why

Apple's kernel clobbers `x18` (platform register) and owns `tpidr_el0`; Horizon's TLR (`tpidrro_el0`), SVCs and the
system counter have no Darwin counterpart (`patch_nso.py:12-22`). iOS forbids writable code, so the rewrite is static and
never patched at runtime; thread-slot indices come from a read-only "key page" (`patch_nso.py:21, 245-248`).

### 2.2 What is rewritten (linear scan of `.text`, `patch_nso.py:609-619`)

| Instruction | Encoding mask | Replacement |
|---|---|---|
| `svc #n` | `w & 0xFFE0001F == 0xD4000001` | host gate, `arg = imm16` |
| `mrs xN, tpidr_el0` | `0xD53BD040` | load thread slot `KTLS` |
| `msr tpidr_el0, xN` | `0xD51BD040` | store `KTLS` |
| `mrs xN, tpidrro_el0` | `0xD53BD060` | load `KTLR` (Horizon ThreadLocalRegion ptr) |
| `mrs xN, cntpct_el0` | `0xD53BE020` | host gate (timer) |
| `mrs xN, cntvct_el0` | `0xD53BE040` | host gate (timer) |
| any `x18`/`w18` operand | capstone regex | load/modify/store slot `K18` via scratch regs |

`x18` handling variants: plain (`site_plain`, 331-341), conditional branch on x18 (`site_cond`, 343-358), `adr/adrp x18`
(`site_pcrel`, 360-369), `br x18` jump tables with landing pads (`site_br`, 371-391; 1.4.3 adds signed 32-bit offset
tables at five sites, `SIGNED_TABLES` 631-637 keyed by build ID `277178b7…`), `blr x18` call windows (`call_window.py`,
1.0.0 `main+0x9c9858`), LDXR/STXR exclusive windows containing x18 copied whole into the patch area so no stores land
inside the reservation (`find_windows`/`site_window`, 457-582). Function bounds from `.eh_frame_hdr` (584-605).
Local TotK fixes vs upstream: jump-table look-back 8 -> 32 instructions for `main+0x590574` (1.0.0)
(`third_party/d2r/README.md`, `patch_nso.py:649`); exclusive window fallthrough exit fix for `main+0x72afdc`,
`main+0x7aa630` (`patch_nso.py:560-562`).

Not rewritten in code but handled at runtime: `mrs CTR_EL0` and `dc civac` trap on Darwin and are emulated in the
SIGILL handler, SDK only (`games/totk/runtime/loader.c:370-374`).

The `CUTS`, `OBS`, `PARAMS` tables in the copied rewriter are D2R-only (keyed by D2R build ID `db0507f3…`,
`patch_nso.py:54-167, 916-928`); TotK uses none of them. TotK game patches live in the runtime (section 6).

### 2.3 Counts (OS touch-points)

- 1.2.1 main: 31,475 rewrite sites; sdk: 100 SVC/timer gates (`docs/v1.2.1-port.md`). sdk has 33 `cntpct` sites,
  14 with destination x19..x23 (`runtime/gate.S` comment at the x0..x29 restore).
- 1.1.2: 55,018 site checks across modules (`docs/how-it-works.md:130`). 1.4.3: 68,968 semantic sites, 554,281
  relocations applied at load (`docs/v1.4.3-port.md:20-21`).
- Game `main` itself makes almost no SVCs; the SVCs live in `sdk`. `main` touches the OS mostly through `x18`
  (compiler uses it as a general register), TLS (`tpidr_el0`) and imported `nn::*` / `nvn*` functions.

### 2.4 Implication for an x86-64 recompiler

- `x18` is an ordinary GPR in Nintendo's ABI: a recompiler must model it as a normal register; no special handling.
- `tpidr_el0` (user TLS, written by the game) and `tpidrro_el0` (TLR, read-only) need per-guest-thread storage; TLR
  layout: host allocates 0x200 bytes TLR, 8 KiB TLS, and stores a dummy pointer at TLR+0x1F8 (`loader.c:277-290`).
  IPC message buffer is in the TLR (first 0x100 bytes, `runtime/ipc.c:16`).
- `cntpct_el0`/`cntvct_el0` must return 19.2 MHz ticks: host computes `ns * 12 / 625` (`loader.c:299-300`).
- Exclusive load/store pairs (LDXR/STXR, LDAXR/STLXR) appear in both game and SDK; a recompiler must lower them to
  host CAS or equivalent with matching semantics.
- Jump tables via `br x18` with byte/halfword/signed-32 offsets exist; static recompilation must recover them
  (the rewriter's recovery logic and the 1.4.3 table list are a ready reference).
- The SVC list in section 3.2 is the complete kernel surface actually used.

## 3. Horizon host

Files: `games/totk/runtime/` (9,856 lines total). Generic parts adapted from d2r-mobile `dm/host`.

### 3.1 Entry and gate

- `runtime/gate.S` (124 lines, ARM64 asm): switches to the per-thread host stack, saves x0-x30, q0-q31, NZCV, FPSR, FPCR
  into an 800-byte frame, calls `runtime_dispatch(return_pc, regs)`, restores (x18 skipped).
- `runtime_dispatch` (`loader.c:296-304`): finds the gate by return address in `<module>.gates.tsv`; timer gates write
  19.2 MHz ticks to the target register; SVC gates call `runtime_svc` (`services.c:304`).
- Host functions handed to guest code run on the thread's host stack (`runtime/hle_stack.c`, `hle_stack.S`, 4096 thunks).

### 3.2 SVCs implemented (complete list)

`runtime/services.c:304-382`, `runtime/threads.c:1-20, 354-560`, `runtime/ipc.c:169-210`. Unknown SVC = fatal.

| SVC | Name | Where | Notes |
|---|---|---|---|
| 0x01 | SetHeapSize | services.c:369 | 2 MiB granularity, cap 4 GiB; 1.4.3 sets ~2.2 GiB in nninitStartup |
| 0x02 | SetMemoryPermission | services.c:315 -> loader.c:66 | RELRO on module data, 4 KiB logical on 16 KiB host pages |
| 0x03 | SetMemoryAttribute | loader.c:91 | only the RELRO lock (mask=attr=16) |
| 0x04/0x05 | MapMemory/UnmapMemory | services.c:131 | stack moves; copy + protect source |
| 0x06 | QueryMemory | services.c:343 | synthesizes MemoryInfo for heap, alias, stack, modules |
| 0x08-0x0A | Create/Start/ExitThread | threads.c:354-378 | |
| 0x0B | SleepThread | threads.c:379 | 0/-1/-2 = yields |
| 0x0C/0x0D | Get/SetThreadPriority | threads.c:409-410 | prio 28..59 |
| 0x0E/0x0F | Get/SetThreadCoreMask | threads.c:411-418 | bookkeeping |
| 0x10 | GetCurrentProcessorNumber | threads.c:419 | virtual core |
| 0x11/0x12/0x17 | SignalEvent/ClearEvent/ResetSignal | threads.c:506 | |
| 0x15 | CreateTransferMemory | ipc.c:202 | |
| 0x16 | CloseHandle | threads.c:549 | |
| 0x18 | WaitSynchronization | threads.c:522 | |
| 0x19 | CancelSynchronization | threads.c:542 | |
| 0x1A/0x1B | ArbitrateLock/Unlock | threads.c:422-430 | tag = owner handle, bit 0x40000000 = waiters |
| 0x1C/0x1D | WaitProcessWideKeyAtomic/SignalProcessWideKey | threads.c:431-469 | condvars |
| 0x1F | ConnectToNamedPort | ipc.c:188 | only `sm:` |
| 0x21/0x22 | SendSyncRequest(+WithUserBuffer) | ipc.c:193 | in-process CMIF/HIPC/TIPC server |
| 0x25 | GetThreadId | threads.c:420 | |
| 0x26 | Break | services.c:377 | |
| 0x27 | OutputDebugString | services.c:378 | |
| 0x29 | GetInfo | services.c:317-338 | ids 0-8,10-18,20-23,28; 18 = program id `0x0100f2c0115b6000` |
| 0x2C/0x2D | MapPhysicalMemory/Unmap | services.c:339, 159-191 | alias region, 4 KiB page bitmap; LimitReached `0x10801` on pool exhaustion |
| 0x34/0x35 | WaitForAddress/SignalToAddress | threads.c:470-500 | |
| 0x45 | CreateEvent | threads.c:501 | |

Semantics follow Atmosphère mesosphère (KConditionVariable, KAddressArbiter, KSynchronization) (`threads.c:12`).

### 3.3 Services

Only one real IPC service is served in-process: `audren:u` (`runtime/audio_renderer.c:343`), plus `sm:` itself.
Everything else is replaced at the `nn::*` function level by mangled-name override during relocation
(`loader.c:209-228`: `runtime_observer` for JUMP_SLOT, then `runtime_override`, wrapped to run on the host stack).
~163 distinct symbols across:

| Area | File | Count | Examples |
|---|---|---|---|
| oe/aoc/pctl/err/diag/fs init | `services.c:289-302` | 24 | GetDesiredLanguage, GetOperationMode, GetPerformanceMode, aoc list = empty |
| fs | `fs_host.c`, `romfs.c` | 31+2 | MountRom, MountSaveData, MountCacheStorage, file/dir ops; saves = host dir |
| hid | `input.c`, `hid_feedback.c` | 16+13 | GetNpadStates (FullKey/Handheld/JoyDual/...), six-axis, vibration |
| applet/oe/nfp/err | `applet_host.c` | 31 | notification messages, launch params, nfp (amiibo) stubs |
| time | `time_host.c` | 13 | clocks, ToCalendarTime, ClockSnapshot (0xD0 layout) |
| codec | `opus_codec.c` | 7 | HardwareOpusDecoder -> libopus |
| movie | `movie_host.c` | vtable | `movie::Decoder` (subsdk0 = NintendoSDK_movie) -> libvpx VP9 |
| pl | `shared_font.c` | 3 | shared font address/size/state |
| swkbd | `swkbd_host.c` | 7 | ShowKeyboard -> host text entry |
| account | `account.c` | 6 | preselected user, nickname |
| vi | `frontend.m` | 5 | OpenDefaultDisplay, CreateLayer, GetNativeWindow, vsync event |
| os | `threads.c`, `services.c` | few | CreateSystemEvent, AllocateAddressRegion (observed) |
| nvn | `hle_nvn.c` | 523 | via `nvnBootstrapLoader` |

Startup: host calls the game's own `nn::init::Start(main_thread_handle, 0, rtld_cb, module_inits, module_finis)` in sdk
(`loader.c:470-477`); DT_INIT for each module is run from the callback (`loader.c:458-462`). nnMain return PC is a
version-table checkpoint.

TotK-specific vs generic: thread/sync/IPC/gate/stack code is generic (adapted from D2R). Memory layout constants,
`nn::*` override list (driven by what TotK `main` imports), audio REV11/REV15 renderer, movie decoder, fonts, swkbd hook
by PLT/GOT address are TotK-shaped. Roughly half of `runtime/` is game-agnostic Horizon emulation.

### 3.4 Threading model

- One guest thread = one host pthread running on the guest stack the SDK allocated (`threads.c:4-14`).
- All waits under one global mutex `G`, one condvar per thread; correctness first.
- Virtual cores: TotK keys per-core allocators/job queues by `GetCurrentProcessorNumber` without locks, so guest threads
  must hold one of 3 virtual application cores while running guest code; core released only where Horizon would switch
  (waits, sleep, yield, exit, blocking host calls). `TOTK_FREE_CORES=1` disables (`threads.c:120-150`). **Critical
  correctness constraint for any port.**
- Guest threads run at `QOS_CLASS_USER_INTERACTIVE` (`threads.c:235`).

### 3.5 Memory layout

`runtime/services.c:18-123`, `docs/how-it-works.md:27-32`.

- All guest memory must be below 2^39: the SDK encodes IPC buffer addresses in 39 bits (HIPC descriptor bits 36..38).
- macOS 26: alias region fixed at `0x70_0000_0000`, size 64 GiB (real Horizon 39-bit size). Stack region 256 MiB, heap
  region below it; 6 GiB low reservation searched from `0x3_0000_0000`.
- Fallback (macOS 27/iOS): one hole for stack+heap+alias, alias shrunk in 4 GiB steps down to 8 GiB; game uses < 3 GiB.
- Application pool reported: `0xCD500000` (~3.2 GiB, retail) + extra for 2K heaps (`services.c:60-63`).
- System resource 32 MiB; NPDM declares 24 MB system resource, so the game uses the alias region via MapPhysicalMemory
  (`refs/nxnative/dm/games/0100F2C0115B6000/game.tsv`).
- Module images: page-aligned, main gets one extra writable page after its image for patch data (`loader.c:182-185`).
- Host page size 16 KiB asserted (`loader.c:489`); Windows has 4 KiB pages / 64 KiB allocation granularity, which makes
  guest-exact 4 KiB permission tracking easier than on macOS.

### 3.6 TLS

Per thread: 8 KiB TLS block (`tpidr_el0` target), 0x200-byte TLR (`tpidrro_el0` target), dummy pointer at TLR+0x1F8,
256 KiB sigaltstack (`loader.c:269-290`). Slot storage = Darwin pthread keys read through `tpidrro_el0 + key*8`.

## 4. NVN layer (NVN on Metal)

Location: `games/totk/third_party/d2r_gfx/gfx/nvn/` and `gfx/tex/`. Mapping reference:
`refs/d2r-mobile/dm/gfx/nvn_metal_mapping.md` (78 lines, good one-page spec).

### 4.1 Files and size

| File | Lines | Role | Backend |
|---|---|---|---|
| `hle_nvn.c` | 2,727 | API front: 258 `NVN(...)` implementations, object semantics, command recording | agnostic C (uses mach for VM, pthread) |
| `totk_nvn_ext.c` | 275 | NVN 55 extras (debug labels, deferred binds, timestamps) | agnostic |
| `nvn_obj.h` | 203 | in-place object layouts + command record format | agnostic |
| `include/nvn.h` | 1,756 | NVN types/structs | agnostic |
| `nvn_protos.h`, `names.txt` | 577, 523 | prototypes, 523 entry names | agnostic |
| `nvn_thunks.S`, `nvn_entry.S` | 6,813, 79 | generated ARM64 counting thunks per entry | ARM64 asm, regenerate |
| `nvn_tex.c`/`.h` | 164 | texture layout sizes (deko3d-derived, zlib) | agnostic |
| `nvn_dirty.c` | 253 | write-protect dirty-page tracking of pool memory | OS VM API (mprotect/mach) |
| `totk_vpool.c` | 121 | virtual memory pools via `mach_vm_remap` | OS VM API |
| `nvn_hmap.h`, `nvn_pool_index.h`, `totk_serial_set.h`, `nvn_cube_group.h`, `nvn_pso_skey.h` ... | ~1,000 | containers, pool index, PSO state keys | agnostic |
| `nvn_mtl.m` | 13,696 | replay of command streams onto Metal 4, PSO cache, render passes, present | Metal / ObjC |
| `totk_mfx.m`, `totk_mfx_up.m` | 621 | MetalFX upscale + frame interpolation | Apple-only |
| `totk_shader_rt.m` | 279 | runtime shader translation client, cache, variants | Foundation + spawn |
| `nvn_refl.m` | 108 | reflection JSON -> bindings | Foundation |
| `fps_overlay.c`, `totk_sysload.c` | 428 | HUD via CoreText, IOKit load | Apple-only |
| `gfx/tex/nvn_deswizzle.c`, `nvn_bcdec.c`, `nvn_bc1astc.c` | ~270 | block-linear deswizzle, BC decode, BC1->ASTC | agnostic |
| `gfx/tex/nvn_deswizzle_gpu.m`, `nvn_bctc.metal`, `nvn_fmt_mtl.c` | ~980 | GPU deswizzle, BC transcode shaders, NVN->MTLPixelFormat | Metal |

Totals: `gfx/nvn` 29,295 lines (incl. 6,813 generated asm); Metal-specific ~15,000; backend-agnostic C ~6,000.

### 4.2 Model

`hle_nvn.c:1-20`, `nvn_metal_mapping.md`:

- `main` imports only `nvnBootstrapLoader`; host overrides it; the NVN driver in sdk never runs. All 523 names resolve to
  counting thunks; unimplemented = fatal (`hle_nvn.c:54-66`). TotK requires NVN API 55, minor >= 15
  (`hle_nvn.c:153`, check at `main+0xe9d614`). Device integer queries return Tegra X1 values (`hle_nvn.c:150-165`).
- GPU address = CPU address. Memory pools are game memory wrapped as zero-copy MTLBuffers (16 KiB-rounded,
  overlapping allowed). Buffer address -> (MTLBuffer, offset) via interval table.
- Commands recorded into the game-provided command memory in `nvn_obj.h` format; replayed at `QueueSubmitCommands`.
  CPU-visible side effects (event writes, counters, sync) happen after GPU completion.
- Sync object = submit fence value; `SyncWait` returns after the GPU reaches it (MTLSharedEvent).
- Textures: block-linear, deswizzled on upload; CPU writes detected by write-protect dirty tracking and re-uploaded.
  TotK streams textures by CPU write + `nvnMemoryPoolFlushMappedRange` (`nxnative game.tsv: tex_flush_upload`).
- Window: game-owned swapchain textures presented by a present thread with `WindowSetCrop`.
- Shader bindings: driver constant buffers c0 (SSBO table) and c2 (texture/sampler handles) are filled by the replay
  layer. TotK offsets: combined texture handles c2+0x20 (32 slots x 8 B), separate textures c2+0x168, samplers
  c2+0x568, images c2+0x120; c0 SSBO tables vs=0x110 ps=0x510, compute c0 0x200 (group counts) / 0x210 (UBO table),
  vertex point-size limit c0+0x610 (`tools/totk_xlate/Service.cs:26-37`); nxnative lists cs SSBO base 0x310 (`game.tsv`).
- Conditional rendering in GPU order (`nvn_mtl.m:1679`), occlusion queries via visibility buffer (`nvn_mtl.m:724`),
  layered passes (`nvn_mtl.m:7252`), store-action lookahead (`nvn_mtl.m:3786-3830`), PSO prediction on submit thread
  (`docs/pipeline-prediction-stalls.md`).
- `nvn_mtl.m` still contains D2R-only paths (shop drag image `8662`, 3D brightening `9948`, world freeze `10380`,
  power-save `10426`): dead weight for TotK.

### 4.3 Retarget to Vulkan or D3D12

Reusable as-is or with small edits: `hle_nvn.c`, `nvn_obj.h`, `nvn.h`, `nvn_protos.h`, `names.txt`, `nvn_tex.c`,
`gfx/tex/*.c`, PSO state key logic, pool index, serial set, the c0/c2 slot conventions, reflection format.
Replace: `nvn_mtl.m` (13.7k lines of ObjC/Metal 4), MetalFX, CoreText overlay, `nvn_dirty.c`/`totk_vpool.c` VM calls
(`VirtualProtect` + vectored exception handler, `MapViewOfFile3`/placeholders for remap), the ARM64 thunk asm
(x86-64 or plain C function table).

Vulkan is the natural target: SPIR-V from the Ryujinx translator feeds it directly (drop SPIRV-Cross and MSL rewriting),
Ryujinx's own Vulkan backend is a working reference for Maxwell-on-Vulkan state mapping, and features Metal lacked
(quads via index emulation, depth clamp, geometry stages, subgroup ops in vertex stage, border colors) are mostly native.
Zero-copy "GPU address = CPU address" maps to `VK_EXT_external_memory_host` or to explicit buffer mirroring with dirty
tracking; on discrete GPUs (no unified memory) the pool-as-buffer model needs staging, which is the biggest design change.
D3D12 would need SPIR-V -> DXIL (spirv-cross HLSL or dxil-spirv); more work, no benefit.

Estimate: a Vulkan replay layer of similar scope to `nvn_mtl.m` (10-15k lines), reusing the front end unchanged.

## 5. Shader path

### 5.1 Translator

- `vendor/ryujinx`: Ryujinx 1.3.2 `Ryujinx.Graphics.Shader`, `Ryujinx.Common`, `Spv.Generator`, unmodified in-tree
  (`vendor/ryujinx/README.md`).
- `games/totk/tools/totk_xlate/`: `Program.cs` (215, IGpuAccessor answering texture/attr/output types from a job file,
  c1 constant segment needed for BRX jump tables), `Service.cs` (468, `--service` line protocol: JSON request ->
  SPIR-V -> spirv-cross MSL + reflection JSON), `VertexCounterLowering.cs` (61).
- `publish_native.sh`: copies vendor/ryujinx, applies patches, `dotnet publish -r osx-arm64 -p:PublishAot=true`.
  For Windows: `-r win-x64`. .NET SDK 10.0.401 used (`prebuilt/macos-arm64/README.md`).
- Patches (`tools/totk_xlate/patches/`), all to Ryujinx shader optimizer, all real correctness bugs, all relevant to any
  SPIR-V consumer:
  1. `0001-keep-call-arguments-in-variables`: don't propagate expressions into Call args (SPIR-V passes args by pointer).
  2. `0002-keep-live-call-sources-when-removing-dead-node`: DCE must not strip sources of side-effecting ops (TotK
     sun-visibility kernel `c7830fac` lost its atomicMin).
  3. `0003-keep-side-effect-assignments-in-the-ast-optimizer`: unused-result atomics (Maxwell RED, ATOM to RZ) kept.
  4. `0004-no-else-if-flattening`: else-if flattening gave shared merge blocks; SPIRV-Cross then dropped the trailing
     else (fragment shader `1870878a` lost loops with atomic max + discard; 12 variants lost atomics, 83 changed discards).
     Note: 0004 is a SPIRV-Cross interaction; check whether the Vulkan path needs it (probably harmless to keep).
- Exact-hash lowerings: visibility-counter vertex shader uses subgroup ballot + atomic; lowered to one atomic add per
  passing invocation, restricted to code SHA-256 `EABE9FDD…24AD` (1.1.2/1.2.1) and `F5F4D231…1FF7B` (1.4.3)
  (`VertexCounterLowering.cs:1-20`). Needed on Metal only because vertex-stage subgroup builtins were unavailable through
  SPIRV-Cross; Vulkan supports subgroup ops in vertex shaders, so likely not needed.
- MSL post-processing in Service.cs: depth textures sampled as plain textures rewritten to `depth2d` etc.
  (`Service.cs:39-46, 118+`), FP64 detection (`Service.cs:47`). Metal-only.
- Window origin upper-left, depth 0..1 (game sets via `nvnDeviceSetWindowOriginMode`/`SetDepthMode`); some shaders use
  `NEAR_IS_MINUS_W` (`totk_shader_rt.m:29`).

### 5.2 Shader identity and extraction

- Key = SHA-256(first 0x880 bytes of NVN control section + code) (`totk_shader_rt.m:1`, `nvn_mtl.m:4962-4963`).
  Control sections are 0x880 or 0x88c bytes; only 0x880 is hashed. Graphics code has a 0x30-byte NVN header
  (+ 0x50-byte SPH inside code per Program.cs) (`scripts/locate_shaders.py`).
- Variant = shader + translator "answers" (texture slot types, image formats, attr/output component types); variants
  reused only if answers match current bind state.
- Sources (`scripts/prepare_shader_seed.py:75-210`, `docs/first-launch-shaders.md` step 1):
  - BNSH files (`BNSH` magic, `grsc` at +96) inside `Shader/*.bfsha.zs` (BFSHA; `material.bfsha` holds most),
    `Effect/*.esetb`, UI layouts.
  - agl `.sharcb` (`BAHS` magic) in `Lib/agl`, `Shader/ApplicationPackage`, `Lib/Terrain`, `Lib/gsys`: records
    `<u64 code offset, u32 control size, i32 control offset>`; controls may be shared. Missing these = magenta title.
  - BNSH embedded in `main`.
  - RomFS `.zs` = zstd with dictionaries from `Pack/ZsDic.pack.zs` (SARC of dicts); SARC nesting up to 4 deep.
- Counts: 1.0.0 9,360; 1.1.2 8,286; 1.4.3 8,290 unique shaders, 0 offline translation failures.

### 5.3 Shared shader list and pipeline cache

- `games/totk/assets/shader_list.tsv`: 41,414 `V` (shader key + deps JSON), 1,557 `S` (libraries to warm), 63 `C`
  (compute pipelines), 4,737 `R` (render pipelines, hex-encoded key), `L` locator rows (`docs/shader-list.md`).
  Identifiers only, no game data. `t` flag = title screen set (1,445 rows across versions), `m` = background warm.
  The `V` rows (shader hash + bind state) are backend-agnostic and directly reusable to pre-translate SPIR-V for Vulkan.
  `R` rows encode Metal-pipeline-specific state keys and are not portable.
- Runtime cache: `~/Library/Application Support/MacNativeGames/TOTK/shader-cache`; missing variants appended to
  `shader-list-contribution.txt`.
- First-compile stalls (100-900 ms) remain the main perf issue; long sessions showed footprint growth 4.5 -> 6.25 GiB
  (`docs/v1.4.3-port.md:55-72`).

## 6. TotK-specific knowledge (CPU-approach independent)

### 6.1 Version address table (`games/totk/runtime/game_version.h`)

Keys are 1.0.0 main offsets; values per version. Generated by `scripts/map_version_sites.py` (instruction-window search
ignoring PC-relative fields; data via ADRP+LDR users; functions via callers) (`docs/v1.2.1-port.md`, "补丁地址").

| Role | 1.0.0 | 1.1.2 | 1.2.1 | 1.4.3 |
|---|---|---|---|---|
| present-interval controller clamp (fps60) | 0x8f6c60.. | 0x8f66e4.. | 0x8f273c.. | 0x2db204.. (`game_patches_v143.c:16-22`) |
| frame-step producer (realtime) | 0x7a83f4.. | 0x7f7a38.. | 0x7ede7c.. | 0x2daa54, 0x3e5658 (`game_patches_v143.c:74-75`) |
| global ratio | 0x8f68b8.. | 0x8f633c.. | 0x8f2394.. | in 0x2daa54 block |
| scene render size store | 0xc630bc | 0xcdd3e8 | 0xcc3f68 | 0x585e48 |
| output size getter call | 0xc630a4 | 0xcdd3d0 | 0xcc3f50 | 0x3dcb28 (display cfg +0x10/+0x508) |
| swapchain size const | 0xe996ac | 0xea7efc | 0xe7aa3c | 0xbdb578 |
| nnMain return checkpoint | 0xe995c4 | 0xea7e14 | 0xe7a954 | 0xbdb48c |
| forceSetMatrix | 0x69a18c | 0x70376c | 0x6ba86c | 0x45ce70 |
| scene slot (data) | 0x4663f08 | 0x4725058 | 0x4728538 | 0x3add238 |
| stale-ref crash site | 0x15ecb0c | 0x1627f10 | 0x1622bf0 | 0x1bd01c4 |
| ShowKeyboard PLT / GOT | 0x2aa4fa0 / 0x453f648 | 0x2b16100 / 0x46129e0 | 0x2b18000 / 0x4615d78 | 0x2bb79f0 / 0x398b348 |
| frame controller return | 0x8f6cbc | 0x8f6740 | 0x8f2798 | 0x2db2ec |
| actor name/pos/rot offsets | 0x210/0x2ac/0x2b8 | 0x218/0x2b4/0x2c0 | same | same |
| ultrawide camera/viewport | - | - | 0xcb4b10, 0x12521cc, 0x19b36f0, 0x1ecdf68 | 0x628c74, 0xb43460, 0x1ff04f4, 0x253fbb0 |

Exact original/replacement words per site: `game_patches.c:27-35, 121-126, 169-173, 298-304`;
`game_patches_v143.c:16-22, 74-75, 193-194, 260-263`. Every patch verifies original words before writing.

### 6.2 Frame timing (the 30 -> 60 fps unlock)

- Adaptive present-interval controller `main+0x8f6bec` (1.0.0), called once per frame from `main+0x8f68b4`, converts
  measured frame work time (system object +0x118) into 60 Hz ticks clamped to [min, max]; result goes to
  `nvnWindowSetPresentInterval` and to the variable frame-rate step `main+0x7a83e4` (ratio = interval*0.5, frame time =
  interval/60). Shipping min = 2 (30 fps). Patch lowers min to 1 (7 instructions) (`game_patches.c:13-35`).
- Real-time step: replace the producer with two floats read from a data page (ratio = dt*30, dt seconds), updated each
  present from a 5-sample moving average of present intervals, clamped to [1/120, 1/15] s (`game_patches.c:42-154`).
- 1.4.3: FrameRateMgr has two call sites fed from `0x3e5658` with a 0.5/1.0 coefficient selected by `sys+0x1da` bit 0;
  replace the nominal 60/FPS value before the multiply, not the callee entry (`docs/v1.4.3-port.md:48`).
- nxnative alternative for 1.4.3 (`refs/nxnative/dm/tools/patch_nso.py:1037-1168`): interval picker `main+0x2db174`,
  int fps at timing +0x50 read at `main+0x2daa54` and `main+0x3e562c`, float fps at +0x24; host chooses whole-vsync
  steps; FPSLocker-derived fixes: D-pad menu fade `main+0x1fbc80`, swapchain argument `main+0xbe14a8` = 1, movie start/end
  30 fps cap at `main+0x2b36100`/`main+0x2b33918`, view render scale at `main+0xd5c24`.
- Side effects of high fps: game-over "Retry" hung on 1.0.0 because `DmF_SY_GameOver` waits one frame after pausing;
  fixed by RomFS overlay. 1.1.2+ use `DmF_SY_GameOver.110` (EventWaitUpdate), unaffected
  (`scripts/make_romfs_patches.py` header). Quick-menu capture background black at 60 fps; fixed by moving bflan
  visibility keys to frame 0.
- `TOTK_PCHTXT` loader accepts community emulator pchtxt patches (by `@nsobid`) (`game_patches.c:387-423`).

### 6.3 Resolution

- Scene render size from parameter tree (`main+0xc6325c`: handheld 1280x720, docked 1600x900), stored in renderer
  +0xcc4/+0xcc8 (`game_patches.c:156-168`). 1.4.3 actual scene RT is 1600x896.
- Output/swapchain 1920x1080 constant before "NvInitializeGraphics" (`main+0xe996ac`); canvas from graphics params
  (+0x38 output, +0x30 scene) (`game_patches.c:207-235`). 1.4.3: display config object +0x10 canvas, +0x508 crop,
  +0x528 scale, pending bit 21 of +8 (`game_patches_v143.c:144-148`).
- Runtime mode switch path: post `nn::oe` message 30 (operation mode changed) + set renderer +0xca4 bit 0x200 ->
  `main+0x1185310` handler relayouts (`game_patches.c:222-230`).
- 2560x1440 needs bigger heaps: `System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs`: `DynamicTextureAllocatorMem`
  113,664 KiB (parent `Graphics` 201,728 KiB); scale by pixel ratio, grow Graphics and the app pool by the same amount
  (+89,088 KiB at 1440p) (`scripts/make_output_res_overlay.py`, `game_patches.c:271-284`).
- UI canvas width cap, 19:9 aspect constants (D2R; not TotK). TotK ultrawide: patch every producer of the projection
  aspect ratio (sead camera +0xb0), DefaultAspect 1.777778 in rodata only copied at camera creation
  (`runtime/aspect_patch.h:1-13`).
- Final upscale shader hashes: EASU `a3a570eb…` (old), `ac58fc18…` (1.4.3, multiplies by 1/3.5 then 3.5, R11G11B10F)
  (`docs/v1.4.3-port.md:39`).

### 6.4 Game data and engine facts

- Engine: sead / agl / gsys, NintendoWare G3d2 / Ui2d / Vfx2, Havok; `main` imports `nn::ro` (NRO loading)
  (`refs/nxnative/dm/games/0100F2C0115B6000/game.tsv`). subsdk0 = NintendoSDK_movie (Android MediaCodec over NvMM).
- Heaps: `sead::ExpHeap` use list at +0xc8, heap self ptr +0x28, size +0x30 (`loader.c:394-419`).
- Movies: 28 WebM in `Event/Movie`, VP9 profile 0, 1920x1080, 30 fps, video only; NVDEC path aborts without a host
  decoder; `movie::Decoder` 17 vtable methods, output format 4 = NV12; pacing by PTS required at >30 fps
  (`runtime/movie_host.c:1-24`). Identical across 1.0.0-1.2.1.
- Audio: `audren:u` REV11 (Q14 biquads) and REV15 (1.4.3: 0x188-byte voice input, float biquads, 0xa8 splitter
  destination) (`docs/v1.4.3-port.md:26`); effects (delay, reverb, compressor) ported from Ryujinx.Audio. Voice streams
  are Opus via `nn::codec::HardwareOpusDecoder` with 8-byte big-endian header {payload size, final range}
  (`runtime/opus_codec.c:1-5`). 1.4.3 English voice: 280,723 packets.
- Streamed audio bug: `content://Sound/Resource/Stream/Dm_OP_0001.bwav` has a double separator after the mount name;
  path parser must skip repeated `/` (`docs/audio-troubleshooting.md`, `runtime/romfs_path.h`).
- Save format (1.1.2/1.2.1): 2,307,656 bytes, header `0x47E0F4`; 54 files in a profile (`docs/v1.2.1-port.md`).
- Language codes the game accepts via `GetDesiredLanguage`: ja, de, fr, es, it, nl, ru, ko, zh-Hans, pt (pt-BR on 1.4.3),
  en-US (`services.c:246-272`).
- RomFS file counts: 1.1.2 304,471; 1.2.1 304,473; 1.4.3 304,243.

### 6.5 Known pitfalls (from docs and loader)

1. Virtual cores: per-core lock-free data in the game; must emulate 3 application cores (section 3.4).
2. 39-bit guest address limit for IPC (section 3.5).
3. MapPhysicalMemory must map only unmapped pages and leave mapped ones intact; TotK heaps grow by remapping overlapping
   ranges. Pool exhaustion must return LimitReached (132 -> `0x10801`), not `0xd401` (abort) (`services.c:164-174`).
4. Texture auto block height: NVN truncates `height_in_blocks*1.5/8`; deko3d rounds up; wrong at boundaries (shrine warp
   pad 160x160 BC3-sRGB shows black) (`docs/how-it-works.md:47-55`).
5. D16 load-clear vs fragment depth rounding differ (0.5 -> 32767 vs 32768); Ultrahand/Recall classification grid
   (`docs/how-it-works.md:57-63`). Check on Vulkan too.
6. Aliased memory (virtual pools remapping physical pools, two pools on same CPU memory) needs alias-aware barriers
   (`docs/how-it-works.md:67-71`).
7. Crash workarounds in game code: stale optional target reference at `GAME_ADDR(0x15ecb0c)` (object+0x6b0) after long
   play (#57); tagged invalid reference `0xffff0000ffffffff` during save load (`loader.c:313-369`). A recompiler must
   reproduce or patch these.
8. TotK reserves only 16 bytes for `NVNtextureView` on its stack (`main+0x823398`), host objects must fit NVN's reserved
   sizes (`nvn_obj.h` FITS asserts).
9. Shader translator optimizer bugs (section 5.1).
10. Long-session metadata growth (dead-serial hash sets ~1 GiB after 3 h) (`docs/serial-history.md`).
11. Mods: RomFS-only overlay via manifest; ExeFS/IPS not supported (`README.md:200`).

## 7. Apple-only vs portable

| Component | Files | Status |
|---|---|---|
| NSO rewriter, gate, thread_enter, hle_stack asm, NVN thunks | `third_party/d2r/*.py`, `runtime/*.S`, `nvn_thunks.S` | ARM64-host-only. Useless for x86-64 execution; valuable as OS-touch census and verification method |
| Loader | `runtime/loader.c` | logic portable (reloc kinds 1027/257/1025/1026, RELR, symbol override); mmap/mach/CommonCrypto/signals Apple/POSIX |
| Kernel emulation | `runtime/threads.c`, `ipc.c`, `services.c` | portable C over pthreads + mach_vm; Windows needs VirtualAlloc2 placeholders, SRWLock/CondVar or winpthreads |
| `nn::*` overrides | `fs_host.c`, `romfs.c`, `time_host.c`, `account.c`, `applet_host.c`, `opus_codec.c`, `movie_host.c`, `shared_font.c`, `swkbd_host.c`, `hid_feedback.c`, `input.c`, `audio_renderer.c`, `audio_effects.c`, `audio_reverb.c` | portable C (libopus, libvpx); CommonCrypto SHA-256 to replace |
| Audio output | `audio_mixer.c` | AudioToolbox; replace with WASAPI/XAudio2/SDL |
| Input, haptics, motion | `gamepad.m`, `gamepad_feedback.m` | GameController/CoreHaptics/IOKit; replace with SDL3/XInput |
| Window, front end | `frontend.m`, `app/launcher.m`, `frontend_ios.m` | AppKit/UIKit |
| Graphics replay | `nvn_mtl.m`, `nvn_refl.m`, `totk_shader_rt.m`, `gfx/tex/*.m`, `.metal` | Metal 4 |
| Upscale / interpolation | `totk_mfx*.m` | MetalFX; replace with FSR/DLSS/XeSS or drop |
| HUD | `fps_overlay.c`, `totk_sysload.c` | CoreText/IOKit |
| Shader translator | `vendor/ryujinx`, `tools/totk_xlate` | portable C#; NativeAOT for win-x64 works |
| Build | `build_app.py`, `scripts/*.py` | mostly portable Python; Apple bits: `xcrun`, codesign, lsregister, APFS clone, `funopen` hactool hook |

## 8. Toolchain

- macOS 26+ on Apple silicon (Metal 4), Xcode command-line tools (`xcrun clang`, C11, `-fobjc-arc`).
- Homebrew: `opus`, `libvpx` (linked statically from `/opt/homebrew/opt/*/lib/*.a`), `python@3.14`.
- Python 3.14 (stdlib `compression.zstd`), venv with `cryptography`, `lz4`, `capstone`, `nsz`; dev: `unicorn==2.1.4`,
  `Pillow`, `brotli` (`requirements-dev.txt`).
- .NET 10 SDK (10.0.401) only to rebuild `totk_xlate` (NativeAOT).
- Fetched at build: hactool @ `1d64a83`, SPIRV-Cross @ `aa217ae`.
- Optional: Xcode Metal toolchain for offline `.metallib`; iOS: Xcode 26, xcodegen, cmake, ninja.
- Single-instance "game slot" lock for all GPU tests (`runtime/game_slot.c`).

## 9. Licenses

From `THIRD_PARTY_NOTICES.md` and LICENSE files:

| Code | License |
|---|---|
| MacNativeGames (repo) | MIT (`LICENSE`) |
| `third_party/d2r` (rewriter, verification), `third_party/d2r_gfx` (NVN on Metal), `tools/totk_xlate` | MIT (d2r-mobile) |
| `nvn_tex.c` texture layout sizes | zlib (deko3d) |
| bcdec 0.985 | MIT or public domain |
| `vendor/ryujinx` (Shader, Common, Spv.Generator 1.3.2) | MIT |
| audio effects/mixer ported from Ryujinx.Audio | MIT (`third_party/licenses/Ryujinx-Audio.txt`) |
| SPIRV-Cross | Apache-2.0 |
| hactool (+ bundled mbedTLS) | ISC (+ Apache-2.0) |
| libopus, libvpx | BSD-3-Clause |
| d2r-mobile | MIT |
| nxnative | MIT (`refs/nxnative/LICENSE`); bundles libvpx, stb_vorbis, libdeflate, Vulkan headers, NVIDIA FLIP with their own licenses |

The TotK-Ultrawide community mod is GPL; MacNativeGames states none of its code is used (`runtime/aspect_patch.h:6`).
Everything we would reuse (host C, NVN front end, translator patches, addresses, hashes) is MIT/zlib/Apache: compatible
with any license we choose.

## 10. nxnative and d2r-mobile notes

- d2r-mobile `dm/host` is the generic origin (loader, `hle_svc.c`, `hle_thread.c`, `hle_table.c` for libc passthrough,
  `HLE`/`HLE_FORCE` registration with mandatory reasons, `UNIMPL` named stubs). Offline shader route: everything
  pre-translated to metallib, no runtime translation. `dm/gfx/nvn_metal_mapping.md` is the best concise NVN spec.
- nxnative generalizes to 14 games with per-game profiles (`dm/games/<title id>/game.tsv`), including TotK 1.4.3 and BotW
  1.9.0 (shares sdk/subsdk0 build with TotK per its comment), Vulkan-on-Metal path for Hades (`dm/gfx/vk/hle_vk.m`),
  LibHac-based `dm/tools/nx_extract` (.NET) for NSP + update layering. Its TotK profile has memory numbers
  (`mem_alias 0x98000000`, heap `0x86000000`, app pool `0xCD500000`) and NVN constant-buffer bases. One-script packaging:
  `scripts/build_ipa_from_nsp.py` (1,229 lines).

## 11. Takeaways for the Windows x86-64 port

1. The CPU strategy must change (no native ARM64), but the rewriter's site census (`sites.tsv`, `gates.tsv` per module)
   is a precise list of every SVC, timer read, TLS access and x18 use; reuse `patch_nso.py`'s scanner as an analysis
   tool for the recompiler front end.
2. The host boundary is mostly at `nn::*` function level, not IPC: a recompiler can link recompiled `main` against host
   implementations of ~163 `nn::*` symbols plus 523 NVN entries, and either recompile `sdk` (for os/thread/heap code
   that uses the 38 SVCs in section 3.2) or HLE more of it.
3. Port as C: `threads.c`, `ipc.c`, `services.c` memory logic, `fs_host.c`, `romfs.c`, `time_host.c`, audio renderer
   and effects, opus, movie, fonts, swkbd, `hle_nvn.c` + `nvn_obj.h` + texture code.
4. Write a Vulkan replay to replace `nvn_mtl.m`; keep Ryujinx translator + the four patches, emit SPIR-V directly.
5. Reuse version hashes, address table, fps/realtime/resolution patch semantics, RomFS overlays, HeapDef math, shader
   extraction formats and the shared shader list `V` rows.
