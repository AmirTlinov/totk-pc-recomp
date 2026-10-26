# Prior art: Switch static recompilation and native ports (as of 2026-10-20)

Scope: prior art for porting TotK (0100F2C0115B6000, ARM64 NSO) to native Windows x86-64 by static recompilation.
Labels: **[confirmed]** = seen in a primary source (post JSON, repo, code). **[reported]** = press or secondary source only.
**[inference]** = my reading of the evidence.

## A. Another TotK PC port (announced 2026-10-15)

- **[reported]** A native PC build of TotK was shown on X on 2026-10-15: 32-59 fps on an RTX 4060 Ti, Vulkan, "worked
  on for a couple of days". No game version, toolchain or source was published, and the screenshots were later
  removed after a copyright report. Press: https://wccftech.com/zelda-tears-of-the-kingdom-unofficial-native-pc-port-sluggish-notrexed-release-2-4-weeks/
- **[inference]** The figures fit a static ARM64-to-C recompilation with an NVN layer on a portable graphics API. There
  is nothing to reuse.

## B. Other projects

### Switch → native, static recompilation (closest prior art)
| Project | URL | Status | Lang / license | Relevance |
|---|---|---|---|---|
| **OpenSmash-Ultimate (ssbu-recomp)** | https://github.com/Denkishi/ssbu-recomp | Active (pushed 2026-10-20), 254 stars. SSBU 13.0.4/13.0.5 → **Windows x64**. Boots, plays full matches at 59-60 fps | C + Python tools, **GPL-3.0** | **Closest blueprint for our target.** Lifter `tools/lifter/recomp.py`: AArch64 → C, one C function per guest function, checked against Unicorn on 32,043 instruction cases. clang → game DLL plus one DLL per script module. Runtime: about 1,100 `nn*` entry points, threads, C++ exceptions through lifted code. NVN → Vulkan/GL (`nvn_vk*.c`). `mx_translate.c`: Maxwell shader → GLSL → SPIR-V. Pipelines prebuilt in background threads. Deterministic mode with a per-frame state hash. GPL: read for ideas; copying code makes our project GPL. |
| **LAS Recomp** | https://github.com/peiikai/LASRecomp (local: `refs/LASRecomp`) | Beta release, 1.37k stars. Link's Awakening v1.0.0 → Windows x64 (Vulkan), macOS (Metal), Linux | C runtime + Python lifter, **MIT** (own code) | **Most reusable code (MIT).** "Made almost exclusively with Claude Code." `tools/lifter.py` (728 lines): AArch64 → C; integer, memory and control flow in C; FP/SIMD lowered to C on x86 hosts by `simd_c.py` (1,156 lines, with `runtime/simd.h`). `gen.py`: whole-NSO generation; functions from `.eh_frame`; Unicorn for analysis; `--chunk`. `tools/test_lift.py` and `test_simd.py`. Runtime: `kernel.c`, `ipc.c`, `services/` (acc, am, audio, fs, hid, time), `nvn/` with a Maxwell translator, Vulkan backend (GLSL via shaderc), ASTC decode. Windows: guest window at `0x70_0000_0000` reserved with `VirtualAlloc`, committed on demand through a vectored exception handler. Toolchain: MSYS2 UCRT64 clang/lld; zig for building on the player's PC. Dev tooling worth copying: `perf_sweep.py` (warp to each room, per-room fps table), `shot.sh`, `frames.sh`, `explore_input.sh`, `fnprof.c`. |
| **SavageWindRecomp** | https://github.com/Revivedx/SavageWindRecomp (local: `refs/SavageWindRecomp`) | Alpha 2 (2026-10-21). Breath of the Wild **1.8.2** to Windows x64 and Linux, Vulkan. Great Plateau at 60 fps; nobody has played past it | C runtime + Python lifter, **MIT** (`LICENSE:1-4`: Revivedx, plus Peiikai for LAS Recomp) | LAS Recomp fork for the engine TotK descends from (sead, ksys, agl; NVN 55.15). Kernel core gate, NVN command recording, submission queue, frame pacing and Vulkan backend were reworked for BOTW; its dated bring-up log is the useful part. Findings: section E. |
| **suyu v0.0.4 "Recompiler mode"** | https://github.com/suyu-emu/suyu-v0.0.4 | Archived after the final release (Sep 2026). No published perf data; users report crashes | C++, **GPL-2.0+** | `src/core/recompiler/arm64_to_c.h` (4.5k lines, header-only): "AArch64 → portable C, N64Recomp-style, GuestContext. Every instruction translates or emits a runtime fallback." Bundled with the Eden/yuzu HLE plus GPU. Useful as a second AArch64→C reference and for its fallback design. Weakest of the three. Coverage: https://xenospectrum.com/en/suyu-static-recompiler-performance-evidence/ |
| **bgyss/SwitchRecomp** | https://github.com/bgyss/SwitchRecomp (local: `refs/SwitchRecomp`) | Specs plus scaffolding. Last push 2026-03, 88 stars | Rust; Cargo says MIT OR Apache-2.0 but the repo has no LICENSE file | See section C. Documentation value only. |
| Mario Kart 8 DX recomp | listed at https://allrecomps.com | "Limited race tests", Windows, no release | ? | Watch only. |

### Switch → Apple native ARM64 execution (no CPU translation; HLE and graphics are reusable)
These do not recompile. They patch the instructions macOS/iOS cannot run (x18, TPIDRRO, `svc`, counter registers) and
run the guest code directly. For an x86 port, the relevant parts are the **Horizon HLE host, the NVN reimplementation,
TotK-specific data and the bring-up playbooks**.
| Project | URL | Notes |
|---|---|---|
| **MacNativeGames** (TotK) | https://github.com/ly05010419/MacNativeGames (local: `refs/MacNativeGames`) | **MIT**, 2.06k stars, active. TotK **1.0.0, 1.1.2, 1.2.1, 1.4.3** on Apple silicon and iOS. Reported about 90 fps on M4 after shader warm-up. NVN → Metal 4 with dirty-page tracking. Shaders: Ryujinx `Ryujinx.Graphics.Shader` (patched, NativeAOT `totk_xlate`) → SPIR-V → SPIRV-Cross → MSL. Shared shader list: 26,832 variants. `docs/how-it-works.md` documents guest memory below 2^39 (IPC buffer addresses use 39 bits), the alias region, NVN block-height rounding (deko3d's heuristic is wrong at some sizes), TotK frame-pacing and 60 fps patches, and per-version expected hashes. **Best TotK-specific reference for HLE and NVN behaviour.** Press: https://www.notebookcheck.net/Zelda-Tears-of-the-Kingdom-Mac-port-runs-at-90-fps-topping-Switch-2-and-PC-rival.1427448.0.html |
| **nxnative** | https://github.com/seedds/nxnative (local: `refs/nxnative`) | **MIT**. 39 game profiles on 2026-10-22 (about 10 new a day), TotK 1.4.3 and BotW at 60 fps. `docs/agents/01..05` is an agent playbook for porting a new Switch title: extract, survey (SDK/NVN version, SVC and import census), bring-up failure catalogue, NVN graphics, debugging; `docs/development.md` has per-game findings. **Best source for project skills and symptom lessons**; what applies to us is in [port-lessons.md](port-lessons.md). The history was rewritten on 2026-10-22. |
| d2r-mobile | https://github.com/jhaizhou-ops/d2r-mobile (local: `refs/d2r-mobile`) | MIT. Origin of the NSO rewriter, Horizon host and NVN-on-Metal design. |
| totk-mobile | https://github.com/OJZen/totk-mobile | Apache-2.0, 73 stars. TotK 1.4.3 only, follows the d2r-mobile design. |

### Decompilation
| Project | URL | Notes |
|---|---|---|
| zeldaret/botw | https://github.com/zeldaret/botw | BotW 1.5.0 decomp, C++, no license file, active (2026-10-19), progress at https://botw.link/progress. Not near 100% (about 40 MB executable). Useful for TotK: shared EPD engine (sead, ksys, agl), so class layouts and names carry over to some extent. Matching decomp is not a viable route for TotK. |

### Design references from other consoles
| Project | URL | Notes |
|---|---|---|
| N64Recomp | https://github.com/N64Recomp/N64Recomp | MIT, 8.3k stars. Canonical "instruction → C over a context struct" model; the suyu engine copies it. |
| XenonRecomp / XenosRecomp | https://github.com/hedge-dev/XenonRecomp , https://github.com/hedge-dev/XenosRecomp | MIT. PPC → C++ recompiler plus Xenos shader → HLSL. Patterns: function-table dispatch for indirect calls, jump-table detection config, mid-asm hooks. UnleashedRecomp (GPL-3.0) is the full-game example. |
| ReXGlue SDK | https://github.com/rexglue/rexglue-sdk | 1.1k stars, active 2026-10. X360 recomp runtime built on a Xenia-derived runtime. Model for "one SDK, many titles". |
| Mario Kart Wiicompiled | https://www.dsogaming.com/news/unofficial-pc-port-of-mario-kart-wii-coming-in-august-2026/ | Wii (PPC) static recomp, AI-assisted, beta 2026-08. |

### Emulators (HLE and GPU references; most are under DMCA pressure)
| Project | URL | Notes |
|---|---|---|
| Ryujinx / Ryubing | GitHub `Ryubing/Ryujinx` returns 404; `ryujinx-mirror` was blocked by DMCA on 2025-03-14. Ryubing's own Git host (not verified) | MIT C#. `Ryujinx.Graphics.Shader` (Maxwell → SPIR-V/GLSL) is the shader translator MacNativeGames and nxnative ship. HLE services are a semantics reference. Reported to be in maintenance mode in 2026. |
| Eden | https://git.eden-emu.dev/eden-emu/eden | yuzu descendant, GPL. Reported as the most active fork; suyu 0.0.4 is based on it. Hit by DMCA in Feb 2026 ([reported]). |
| Citron, Sudachi | (off GitHub) | Citron is alive off GitHub; Sudachi is mostly dead ([reported], low-quality sources such as https://tech-insider.org/eden-vs-ryubing-best-switch-emulator-2026/). |
| dynarmic | https://github.com/lioncash/dynarmic (archived 2024), forks https://github.com/azahar-emu/dynarmic and in-tree in Eden | 0BSD. A64 decoder tables and IR are a semantics reference for the lifter (flags, FP/SIMD corner cases). |
| oaknut | https://github.com/merryhime/oaknut | MIT, archived 2024. AArch64 emitter/encoder, not a decoder. Low relevance. |

### Generic lifters
| Project | URL | Notes |
|---|---|---|
| remill | https://github.com/lifting-bits/remill | Apache-2.0, active 2026-08. AArch64 → LLVM IR. Per-instruction semantics as C++ functions. Candidate for an LLVM-IR route or as a semantics oracle. Its AArch64 SIMD coverage is incomplete (from memory, not checked against the current tree). |
| mcsema | https://github.com/lifting-bits/mcsema | AGPL-3.0, archived 2022. Do not use. |
| RetDec | https://github.com/avast/retdec | MIT, LLVM decompiler. Analysis aid only. |
| Unicorn | https://www.unicorn-engine.org | Differential-test oracle; LASRecomp and ssbu-recomp both use it. |

### What the field converged on (all three working Switch recomps)
1. AArch64 → **C** (not LLVM IR). One C function per guest function, chunked source files, compiled by clang.
2. Function discovery from `.eh_frame`, `bl` targets and relocations. Indirect calls go through a dispatch table
   keyed by guest address.
3. Guest memory at its original addresses, reserved up front (`0x70_0000_0000` window). Addresses must stay below 2^39.
4. Per-instruction differential testing against Unicorn.
5. HLE of `nn::` services and SVCs in the host runtime, not kernel emulation.
6. NVN reimplemented at the API level. Maxwell shaders translated at load time (own translator or Ryujinx's).
   Pipelines prewarmed.
7. Dev loop: scripted input, warp sweeps, per-scene fps tables, frame dumps, function profiler.

## C. refs/SwitchRecomp assessment

Layout: `specs/` (35 draft specs SPEC-000..270), `docs/` (35 notes and schemas), `crates/` (8 crates, about 15.9k lines
of Rust including tests), `skills/` (7 Codex skills), `scripts/` (capture, ffmpeg, Ghidra export, AWS). There is one
commit in the local clone, dated 2026-02-17.

### Crates: usable? No.
Not compiled here: `cargo` is not installed and installing it was out of scope. Dependencies are pure Rust (clap,
serde, toml, sha2, lz4_flex, thiserror), so it would most likely build and its tests would pass, but nothing in it does
real work.
- `recomp-isa`: a toy interpreter covering 12 ops (`MovImm`, `Add`, `Sub`, `Cmp`, shifts, `Ldr/StrImm`, `Ret`) with
  registers in a `BTreeMap`.
- `recomp-pipeline/src/homebrew/lift.rs` (1.2k lines): decodes about 15 integer encoding classes (movz/movk, adr,
  add/sub imm/reg, logical reg, ldr/str unsigned imm, b, bl, b.cond, cbz/cbnz, tbz/tbnz, br, ret, nop). It has no
  LDP/STP, pre/post-index, FP/SIMD, exclusives or atomics, system registers or `svc` lowering. That covers a negligible
  fraction of a real game's .text.
- Emitter (`output.rs`) writes a Rust crate with **per-function local registers** (`let mut x0: i64 = 0`) and a
  `match block_label` string dispatch. Registers do not cross calls, so the output is semantically wrong for real ABI
  code and slow.
- `recomp-gfx`: `CommandStream` + `GraphicsBackend` trait + `StubBackend` + FNV checksum. No NVN or Maxwell.
- `recomp-services`: a name → closure registry with allow-list and stub behaviours. No IPC or CMIF.
- `recomp-runtime`: flat memory regions, `mem_load/store_*`, syscall log, noop and panic.
- XCI intake shells out to hactool/hactoolnet. Fixtures are fake (`main.nso` is 5 bytes).
- Verdict: do not depend on it. LASRecomp (MIT) is a far better code base to learn from or borrow.

### Specs and docs: limited value
- Process and validation specs (provenance, A/V validation, run manifests, automation loop, agent pipeline security)
  are coherent but generic and were written before any real title booted.
- `docs/llm-assisted-decompilation-long-tail.md` summarizes https://blog.chrislewis.au/the-long-tail-of-llm-assisted-decompilation/
  (similarity-guided retrieval, lanes for gfx/math, hook-enforced guardrails, parallel worktrees). Worth reading if we
  run an agent loop over functions.
- `docs/LEGAL-POLICY.md` and the provenance idea (sha256 of user inputs, inputs kept separate from outputs) are worth
  copying in spirit.

### Skills: adapt one, rewrite the rest from better sources
| Skill | Content | Verdict |
|---|---|---|
| static-recomp-av-compare | SKILL.md + 3 real Python scripts (565 lines): ffmpeg normalize/align, SSIM, PSNR, optional VMAF, EBU R128 loudness, batch manifest, threshold check | **Adapt.** The only skill with working code. Port it to a Claude Code skill for "emulator reference capture vs our build" comparisons. Check the license first (no LICENSE file in the repo). |
| static-recomp-input-replay | Checklist | Concept is needed; the content is generic. Write our own around a frame-indexed input trace fed to both Ryujinx/Eden and our runtime. |
| static-recomp-regression-triage | Checklist mapping symptoms → loader / ISA / services / GPU / audio | Rewrite using nxnative `docs/agents/03-bringup.md` and `05-debugging.md`, which are concrete failure catalogues. |
| static-recomp-perf-profile | Checklist | Rewrite around a LASRecomp `perf_sweep.py`-style warp sweep plus a function profiler. |
| static-recomp-reference-capture | Checklist | Fold into av-compare. |
| static-recomp-scope-plan, static-recomp-batch-harness | Multi-title catalogue planning | Skip. We have one title. |

Better skill sources: nxnative `docs/agents/` (extract → survey → bring-up → graphics → debugging), LASRecomp
`tools/` (test_lift, test_simd, perf_sweep, shot, frames, explore_input), and ssbu-recomp's Unicorn differential tests
and deterministic state-hash mode.

## Takeaways for this project
1. The other TotK PC port (section A): no code, and its screenshots were taken down. Proof of feasibility, not a dependency.
   Its stack (MSVC link + wgpu) is plausible but unconfirmed.
2. Base the lifter design on LASRecomp (MIT, AArch64→C, SIMD→C for x86) and validate it the way ssbu-recomp does
   (Unicorn differential tests). Read suyu's `arm64_to_c.h` for its fallback strategy.
3. Base HLE and NVN behaviour for TotK on MacNativeGames (MIT): per-version hashes, memory layout constraints, NVN
   texture layout fixes, frame-pacing patches, shader-variant list.
4. Shaders: Ryujinx `Ryujinx.Graphics.Shader` (MIT) → SPIR-V is the proven route. LASRecomp and ssbu-recomp wrote
   their own Maxwell → GLSL translators.
5. SwitchRecomp: reuse the av-compare scripts and the legal/provenance policy only.

## D. Performance techniques in other Switch recomps (2026-10-21)

Clones read locally (not in `refs/`): HWDER (https://github.com/sonsegajp/HWDER, MIT), B2recomp
(https://github.com/K0NL/B2recomp, GPL-3.0; its runtime is a copy of ssbu-recomp), SuperMarioWonderRecomp
(https://github.com/BelmanteGu/SuperMarioWonderRecomp, MIT), ssbu-recomp (GPL-3.0). GPL code: read only.

1. FPCR: all four ignore it and run FP on host SSE in the default mode. HWDER stores FPCR and never applies it
   (`tools/recomp/a64.py:371,394`); B2recomp and ssbu drop FPCR writes (`lifter/recomp.py:269-271`,
   `tools/lifter/recomp.py:287-290`); Wonder documents FPCR as "not honoured" (`docs/RECOMP.md:31,71`). None handles
   a game that sets FZ, as TotK does. Exactness work they keep: NaN sign, FRECPE/FRSQRTE (B2recomp `runtime/fpexact.h`).
2. Cores: HWDER and Wonder run guest threads free, one host thread each, no preemption model; GetCurrentProcessorNumber
   returns the ideal core or the lowest mask bit (HWDER `svc.cpp:157-170`, Wonder `runtime/src/kernel.cpp:528-538`).
   ssbu and B2recomp have an opt-in 3-core pinning model, shipped off because it costs frame rate
   (`os_threads.c:143-156`). TotK crashes without our core gate (`TOTK_FREE_CORES=1`), so the gate stays.
3. Waits: ssbu sleeps with a high-resolution waitable timer (`os_threads.c:104-122`); Wonder removed its yield nap
   (it cost the main thread about 18%, `kernel.cpp:81-96`).
4. Codegen: HWDER clang-cl /O2 /arch:AVX2 -mfma, one dense dispatch table per module with a musttail call
   (`core.cpp:26-58,131-138`); Wonder -O2 with `-ffp-contract=off`; B2recomp `-ffp-contract=off`, `optnone` on
   functions over 5000 statements, and a register-caching pass measured at no gain under -O2 (`regcache.py:1-30`);
   ssbu stays at -O1 because huge lifted functions take over an hour at -O2 (`tools/lifter/build.py:55-68`). No LTO
   or PGO anywhere.
5. Pipelines: HWDER compiles on worker threads and skips the draw until ready (`vk_renderer.cpp:1391-1430`); ssbu
   waits by default, but its workers precompile predicted keys and replay `pipes.bin` plus a VkPipelineCache at start
   (`nvn_vk_draw.c:4037-4062`); B2recomp prewarms from a saved list (`nvn_vk_m4.c:1702-1759`); Wonder's canonical
   pipeline key removed 18% of compiles (`docs/nvn/android-perf/PIPELINES-1810.md:15-20`).
6. GPU to CPU data: ssbu copies idle compute buffers on the GPU into host memory and merges them into guest memory when
   the submit completes, avoiding 19-20 ms reads of write-combined memory (`nvn_vk_draw.c:1940-1960`); Wonder splits
   the submit after the last dispatch (`runtime/src/nvn/nvn_frames.cpp:11-19`).
7. Textures: ASTC on the CPU everywhere (Wonder multi-threaded, `runtime/src/nvn/nvn_astc.cpp`); HWDER detects dirty
   textures by content hash with a 48 MB per-epoch budget (`texture_cache.cpp:322-334`).
8. Pacing: HWDER mailbox present plus its own 60 Hz limiter, swap interval 2 paced two slots ahead
   (`display.cpp:160-163,317-334`); Wonder prefers MAILBOX; B2recomp patches 60-120 fps at lift time from a census of
   frame-rate sites (`lifter/fps_ticks.txt`).

## E. SavageWindRecomp findings (2026-10-23, `refs/SavageWindRecomp` @ `575ad88`)

MIT (`LICENSE:1-4`), pinned in `tools/fetch_refs.sh`. 430 files, 8 commits, one week old. Its notes (`docs/SAVAGE-WIND.md`, Spanish, 904 lines, cited
as `SW.md:line` = first line of the entry) are a dated log of BOTW 1.8.2 bring-up fixes. Most are bugs TotK 1.2.1 hits too. Paths are relative to
`refs/SavageWindRecomp/`; our code is cited by file and function, and the Ours column was updated 2026-10-26. BOTW
addresses do not carry over to TotK; the mechanisms do.

| Area | Theirs | Ours | Differs |
|---|---|---|---|
| Cores | 4 tokens, ticket queue per core (`runtime/kernel.c:96-235`); mask used only if single-bit (`kernel.c:867-880`) | 3 cores, mask-aware grant, preempt flag, asynchronous preemption of a holder in game code (ours: `kernel.c` `core_take`, `kern_ap_stub`) | Yes |
| Preemption points | SVCs plus every indirect call (`runtime/dispatch.c:167`) | SVCs and every indirect call (ours: `svc.c`, `dispatch.c` `dispatch_preempt`), plus the waiter's interrupt | No |
| Exclusive monitor | Value CAS (`runtime/cpu.h:73-97`); pairs unsupported (`tools/lifter.py:391-392`) | Same macro plus 128-bit pair (ours: `cpu.h` `EXCL`) | No (ours is a superset) |
| Call/CopyCommands | Record `CMD_CALL`, executed recursively at submit (`runtime/nvn/cmdbuf.c:687-688`, `runtime/nvn/vulkan/exec.c:4689-4695`) | Inlined at record time (ours: `nvn_api_cmd.c` `inline_recording`) | Yes |
| UpdateUniformBuffer | Applied at execution into guest memory, skipped if bytes equal (`exec.c:4863-4871`) | Recorded, written at replay (ours: `nvn_api_cmd.c` `nvnCommandBufferUpdateUniformBuffer`) | Partly |
| Submit | Host queue thread, part-wise GPU submit (`runtime/nvn/objects.c:157-170`, `exec.c:1015-1017`) | Replay on the submitting thread, submitted in parts every 64 draws; a queued replay thread behind `TOTK_REPLAY_ASYNC=1` (ours: `nvn_api_core.c` `nvnQueueSubmitCommands`, `vk_backend.c`) | Partly |
| Frame pacing | Patches VFR and the vsync wait (`tools/patches.py:186-223`) | MacNativeGames' present-interval and real-time frame-step patches, back-edge present pacing (ours: `frame_rate.c`, `vk_backend.c`) | Yes |

### a. Threads
- Core gate like ours: a token per core, best priority then ticket order (`kernel.c:96-165`). Reason given: BOTW shares a sead heap without a lock between threads pinned to one core (`kernel.c:97-103`).
- Four tokens, indexed `core & 3` (`kernel.c:150`); a multi-bit affinity mask is not honoured, only a single-bit mask or the ideal core (`kernel.c:876`). `-1`/`-2` land on core 0 (`kernel.c:388`).
  Ours takes any free allowed core and keeps the ideal core for -2/-3, closer to Horizon.
- Preemption at indirect calls: `rt_dispatch` compares `g_core_best_wait[core]` with the running priority and yields (`dispatch.c:167`, `kernel.c:120-130, 220-229`).
  Reason: a prio-48 job thread ran about 5 ms of guest code without a syscall once a second while a prio-44 worker on the same core, which the main thread waited for, sat waiting (a frame 10 ms longer each second) (`SW.md:540`).
  Measured p99 frame time 23.6-24.8 ms without, 17.4-20.6 ms with (`SW.md:553`). Ours has the same indirect-call check (`dispatch.c` `dispatch_preempt`).
- Host work releases the core: the submit and present path calls `kernel_host_work_begin/end` (`kernel.c:196-205`, `runtime/nvn/objects.c:132, 245-249, 292-294`). Reason: the translation took about 10 ms a frame on the core token (`SW.md:295`).
- Yield and `SleepThread(0)`: release then reacquire, so same-priority waiters go first (`kernel.c:231-236, 838`). `SignalProcessWideKey` and `ArbitrateUnlock` yield after unlocking, not before (`kernel.c:735-741`, `SW.md:256`).
- `svcWaitProcessWideKeyAtomic`: releasing the mutex before marking the condvar hung half the runs (`SW.md:224`); a timed-out wait returns without the mutex, and a timeout racing a wake does not count as expired (`SW.md:253`). Same family as port-lessons S6.

### b. Exclusive monitor
- Value CAS on the address recorded by LDXR, identical to ours (`runtime/cpu.h:73-97`). No ABA protection on either side. Nothing to adopt.

### c. NVN command buffers
- The game sees command memory accounted at an estimated NVN cost per command, `cmd_cost` (`cmdbuf.c:15-28`); records live in host "shadow" blocks keyed by guest memory.
  Reason: BOTW records each display list once to measure it, then allocates exactly that from a heap sized by the ResourceSizeTable for real NVN (`SW.md:201`).
- A list dies only when a recording writes over its guest memory (`shadow_claim`), not when the game adds an overlapping window. The old rule lost the deferred-lighting material lists, so no light was composed (`SW.md:233`). Dead blocks are reused three frames later (`cmdbuf.c:25-26`).
- Textures in records are resolved at record time (`nvn_texture_host`, `cmdbuf.c:442-455`). The exposure chain (192x192 down to 3x3) re-inits one `NVNtexture` per step inside a command buffer; resolved at execution, every step drew into the last (`SW.md:242`).
- One Vulkan image per (memory, shape): `NVNtexture` objects on the same memory and shape share an image (copies, finalized originals still in the pool, re-created render targets) (`SW.md:220, 245`; `runtime/nvn/vulkan/textures.c`).
- `NVNsampler` objects are copied by value and the copy is registered after the original is finalized. Descriptions are shared per configuration and never freed; otherwise the Shrine corridor texture clamped instead of repeating (`SW.md:261`).
- Draws resolve at execution on the queue thread. UBO ranges are copied at the draw, shared per list, and `UpdateUniformBuffer` (about 450 a frame into about 40 ranges) invalidates only the copies over its range; identical rewrites (about 1 in 5) are skipped (`exec.c:471-503, 2415-2430, 4863-4871`; `SW.md:535, 565`).
- `BindTexturesDeferred` was a no-op and dropped the lighting composite's textures; the other `Bind*Deferred` entry points are still unimplemented there (`SW.md:240, 278`).

### d. Engine special cases
- Few guest patches: 17 in `tools/patches.py:209-223`, 15 for their own options screen. The two for us: the sead vsync wait `0x7000d811fc` (`patches.py:186-203`) and the `ksys::VFR::updateInterval` ratio load `0x70014092f4` (`patches.py:206-210`).
  TotK equivalents are in macnativegames.md (present-interval controller, lines 438-446).
- Frame pacing (`runtime/hle/framepace.c`, `runtime/framepace.h`, `SW.md:430`): presents on a fixed grid; the game's clock is told the real frame length through `rt_vfr_update` (ratio = seconds x 30, at most 3 % change per frame, at most 1/20 s per step, debt paid back over later frames) (`SW.md:454`);
  integer interval with hysteresis (1 above 43 fps, 2 below 35, 3 below 23); menus, events and movies pinned to 30 fps (`SW.md:443`); controller hold counters stepped by real time.
  Havok, stamina, time of day and animation all read the ratio; a ratio that alternated made Link's camera judder.
- Dynamic resolution: BOTW picks 1600x900 or 1280x720 from GPU time in `ReportCounter`; their translation time dropped the open world to 720p. Fix: every counter in a frame gets the same timestamp (`exec.c:4890-4902`, `SW.md:414`).
- Render-thread layout: the main thread records about 30 of 62 lists per frame, two workers the rest; the sead render thread (`0x7000d81100`) submits one list a frame and presents (`SW.md:503`).
- FPCR.FZ is set at thread start (`0x7000b4de30`, `0x70000c4e3c`). Software flushing cost about 40 % of main-thread time; they mirror FZ into MXCSR (`runtime/simd.h:53-58, 239`; `SW.md:516`). Ours already mirrors it (ours: `cpu.h` `fpcr_host`).
- Depth clamp: BOTW enables it for the three sun-shadow cascades and the sun query. Ignored, geometry nearer than a cascade's near plane was clipped; tree shadows appeared, were cut and moved with the camera (`SW.md:593`). Ours records and applies it (ours: `nvn_api_cmd.c` `nvnCommandBufferSetDepthClamp`, `vk_pipeline.c` `depthClampEnable`).
  `SAMPLES_PASSED` returns 0 and `SetRenderEnableConditional`/`SetRasterizerDiscard` are no-ops (`SW.md:593`).
- Same-pass feedback: the terrain fragment shader ORs bits into the G-buffer's own pixel, about 240 draws a frame. Copying the 1600x900 target before each draw cost 8.5 of 16 ms GPU. They use `VK_EXT_attachment_feedback_loop_layout` plus `VK_KHR_dynamic_rendering_local_read` with an in-pass barrier: copies 8.5 to 0.4 ms (`SW.md:325`).
  Ours does the same since 2026-10-23 (`TOTK_FEEDBACK`, [maxwell.md](maxwell.md) check 10); TotK has 425-667 such draws.
- Submit in parts: a pass boundary about every 300 draws (`exec.c:1015-1017`, `SW.md:302`), except after a shader storing to guest memory (about 10 dispatches a frame). Ranges those storage buffers can write are tracked; a UBO inside them is read in place from the imported heap, and any other CPU copy of that memory waits for the GPU (`SW.md:524`). Holding the rest of the list caused device-lost in 1 of 7 loads when copying half-written data.
- Present on its own thread with a second queue; NVIDIA `vkQueuePresentKHR` blocked on the GPU for 13-18 ms in one frame of five (`runtime/nvn/vulkan/device.c:563-593`, `SW.md:306`).
- Shaders and pipelines: translation cached in memory and on disk (`cache/xlate/`, `SW.md:309`); `vk_pipelines.bin` with a GPU/driver/translator header plus `vk_pipelines.list`, replayed by worker threads at start-up, first world frame waits for it (`SW.md:337`, `device.c:310-384`).
  Scripted walk, render-thread compile time 5898/865/362/449 ms to 16/1/32 ms, frames over 40 ms 56/51/9/11 to 24/4/5. The pipeline key held colour formats and blend bytes of unused targets, giving about 1,500 pipelines where about 870 were needed (`SW.md:364`).
- Synchronous shader compile by default: a draw skipped while its pipeline builds is lost for good if the game draws it once (save thumbnail) (`SW.md:657`).
- Targets are not cleared per frame (BOTW keeps data in them between frames); new images start at zero (`SW.md:409`).

### e. Known glitches
Issue list (`gh issue list -R Revivedx/SavageWindRecomp --state all`, 23 issues, 2 closed, none with a technical fix thread for graphics):
- #16 trees "vertex explosions" at some distances or angles, 4K, no upscaler. Open; a collaborator replies "almost fixed". No cause stated. Closest match to one-frame popping.
- #20 distant LODs not rendering fully. Open, no analysis. #26 and #5 "texture errors" and "graphic errors" (RX 570 and unknown GPU). Open, logs requested.
- #23 ROG Ally: 114-700 ms frames on transitions and shader generation, up to 250,000 draws reported per frame, VRAM use 0.4-1.4 GB. Open. Matches their own note that a first-use pipeline costs 40-100 ms (`SW.md:386`).
- #31 30 fps in menus, chests, cutscenes: by design (`framepace.h:15-17`). #30 asks to precompile all shaders; the pipeline list is their answer.
- Crashes with a stated cause: #2 voice banks swapped in are larger than the ResourceSizeTable entry, so a later heap allocation fails; #6 `movie.c` vtable slot 2 is a stub, crash at the first tower cutscene.
Fixed in `SW.md` and relevant to TotK: stretched texture from sampler copies (`SW.md:261`), blocky image from re-created render targets (`SW.md:220`), black cube arrays (`SW.md:248`), moving tree shadows from depth clamp (`SW.md:593`), missing lighting from dead command lists (`SW.md:233`).
Still open in their log: a differently tinted rectangle top-left from half-resolution fog passes (`SW.md:271`), a 32x32x32 BC4 3D texture sampled as 2D returning black (`SW.md:276`), an ambient palette that fades interior to exterior over 60 s (`SW.md:606`).
Nothing in the issues or the log describes objects teleporting for a frame, or a wrong-LOD mesh, with a cause.

### f. Linux platform layer
- `runtime/platform.h` is the split: `plat_reserve_guest`, `plat_commit`, `plat_reset_guest`, `plat_protect`, `plat_now_ns`, `plat_sleep_until_ns`, spawn, symbols, crash handler (`platform.h:16-65`). Implementations: `runtime/platform/{windows,linux,macos}.c`, with `win_compat.h` and `linux_compat.h` filling POSIX gaps.
- Memory: Linux maps the whole guest window once, `MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE` (`platform/linux.c:29-33`). Windows reserves with `VirtualAlloc` and commits in chunks from a vectored handler (`platform/windows.c:20-27, 71-76`).
- Write watching is a debug tool: page `PAGE_NOACCESS` plus single-step (`windows.c:30-58`, `runtime/kernel.c:1150-1160`). The renderer does not track dirty pages; it copies guest memory when the draw runs.
- Threads are pthreads on both (winpthreads on mingw). Timers: `clock_nanosleep(TIMER_ABSTIME)` on Linux (`linux.c:54-60`); a high-resolution waitable timer plus `timeBeginPeriod(1)` on Windows (`windows.c:108-125, 205`).
- Linux needs a 4 KB or 16 KB page kernel (`docs/linux.md`).

### What to adopt, ranked by expected effect on glitches and stutter

Status 2026-10-26: 1 and 2 are in, 3 is in part (part-wise submit; the queue is off by default), the feedback-loop
layout is in.

1. Preemption at indirect calls in `rt_dispatch`, plus the host-work core release. Small change, removes a periodic 5-10 ms frame spike they measured (p99 23.6-24.8 to 17.4-20.6 ms). Needs TotK confirmation with our sched trace.
2. Persistent pipeline list pre-warmed at start-up, with a key that holds only the used targets. Removes the 40-100 ms first-use hitches that read as stutter (frames over 40 ms 56 to 5 on their walk).
3. Host submission queue with part-wise GPU submit (about 300 draws) and the guest-memory-write tracking for storing shaders. Their p99 went 38-43 ms to 29-34 ms with queue, MXCSR FZ and range tracking together (`SW.md:550-552`).
Next: feedback-loop layout instead of target copies, one image per (memory, shape), UBO copy sharing with per-range invalidation, equal-timestamp counters for dynamic resolution.
