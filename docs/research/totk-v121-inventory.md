# TotK 1.2.1 module inventory

Numbers measured on a retail 1.0.0 base plus the 1.2.1 update (`v393216`), extracted with `tools/extract.py` on 2026-10-20. Counts only; no game data.

## Input verification

| | Result |
|---|---|
| Base ExeFS (`main`, `sdk`, `subsdk0`, `rtld`) | match `expected-v1.0.0.json` |
| Base RomFS | 17,069,139,640 bytes, 304,207 files, SHA-256 matches; 1,041,818 level-5 IVFC blocks verified |
| 1.2.1 ExeFS including `main.npdm` | match `expected-v1.2.1.json`; ACID signature verifies |
| 1.2.1 RomFS (BKTR merge) | 17,052,047,360 bytes, 304,473 files, SHA-256 matches |
| MacNativeGames rewrite of `main`, `sdk`, `subsdk0` | output hashes equal `rewritten_sha256` in `expected-v1.2.1.json` |

The tested base dump's `main.npdm` differs from the reference only in bytes `0x80-0x27F` (ACID signature and ACID public key), so its ACID signature no longer verifies. A repacking tool that re-signs the NCA header does this. Code and RomFS are unaffected.

Timing on the development machine (NVMe): ExeFS under 1 s each, base RomFS 40 s, update merge with NCA copies 1 min 50 s.

## Modules

`image` is the loaded size (all segments plus `.bss`, page aligned).

| Module | image | `.text` | `.rodata` | `.data` | `.bss` | relocations | imports | exports |
|---|---|---|---|---|---|---|---|---|
| `main` | 74,625,024 | 45,193,808 | 23,658,396 | 5,006,256 | 765,008 | 510,904 | 675 | 29 |
| `sdk` | 14,520,320 | 6,161,120 | 7,150,872 | 429,712 | 774,512 | 35,032 | 13 | 27,747 |
| `subsdk0` | 6,983,680 | 3,424,448 | 2,202,004 | 199,416 | 1,152,264 | 19,397 | 667 | 10,954 |

`main` exceeds LAS Recomp's 64 MB module limit (see [lasrecomp.md](lasrecomp.md), "Scaling risks").

## Functions and instructions

LAS Recomp `tools/analyze.py --census` (seeds: `.eh_frame_hdr`, exported FUNC symbols, `R_AARCH64_RELATIVE` targets in `.text`, `BL` targets; each start is credited to the first source that finds it).

| Module | functions | eh | sym | ptr | bl | instructions | undecodable words | mnemonics | time |
|---|---|---|---|---|---|---|---|---|---|
| `main` | 247,495 | 246,909 | 0 | 3 | 583 | 11,298,063 | 389 | 259 | 15 s |
| `sdk` | 38,305 | 31,238 | 36 | 8 | 7,023 | 1,539,998 | 282 | 362 | 3 s |
| `subsdk0` | 13,690 | 10,061 | 15 | 2 | 3,612 | 855,798 | 314 | 178 | 1 s |
| total | 299,490 | | | | | 13,693,859 | | | |

About 2.5-3 times *Link's Awakening* (4-6 M instructions). `main` has unwind info for nearly every function, so `.eh_frame_hdr` alone gives a near-complete function list.

Instructions the lifter must cover, by module:

- Exclusive pairs (`LDXP`, `STXP`, `LDAXP`, `STLXP`): `sdk` only (26 / 38 / 50 / 63). `main` has none.
- LSE atomics (`CAS*`, `CASP*`, `SWP*`, `LDADD*`, `LDCLR*`, `LDEOR*`, `LDSET*`): `sdk` only, 1-2 of each variant, likely behind a CPU feature check.
- Crypto: `sdk` uses AES (`AESE`, `AESD`, `AESMC`, `AESIMC`), SHA-1 and SHA-256 instructions. CRC32: `main` 7,710, mostly `CRC32B`.
- Exclusive single (`LDXR`/`STXR`): `main` 15,821 / 16,308; acquire/release forms in all three.
- System: `SVC` 60 (`sdk`), `MRS` 865 / `MSR` 53 (`main`), `MRS` 181 / `MSR` 13 (`sdk`).
- Indirect branches: `BLR` 105,284 and `BR` 26,426 in `main`.
- SIMD: `FMLA` 24,520, `TBL` 2,327, `LD1`-`LD4`/`ST1`-`ST4` in `main`.

## Lifter coverage

`tests/diff/lift.py --coverage` lifts every distinct word of `.text` (`recomp/lifter.py`, portable FP/SIMD lowering). Every word is lifted except those that become an `rt_udf` trap:

| Module | distinct words | trapped words (distinct) | of which `UDF` | undecodable data (capstone `?`) | other |
|---|---|---|---|---|---|
| `main` | 1,393,284 | 28,266 (10) | 27,873 | 389 | 4: the module header at `main+0x0`-`0x20`, which capstone decodes as SVE |
| `sdk` | 313,813 | 22,494 (9) | 22,212 | 282 | 0 |
| `subsdk0` | 233,022 | 5,858 (8) | 5,545 | 313 | 0 |

Upstream LAS Recomp rejects 277 `sdk` words: the exclusive pairs and the LSE atomics. Both were added (`excl_pair`, `cas`, `casp`, `atomic` in the lifter; `ldxp128`/`stxp128` and `atomic_[su]{max,min}N` in `runtime/cpu.h`).

Differential test results (2026-10-20, seed 1):

| Test | Encodings | Cases | Failing |
|---|---|---|---|
| `lift.py` (all opcode signatures of the three modules, then random words; 20,000 per module) | 60,000 | 239,948 | 0 |
| `simd.py` (1,048,640 FP/SIMD encodings: lifter and Unicorn accept the same 270,299) | 20,000 | 159,976 | 0 |
| `atomic.py` (exclusive sequences, LSE, CAS, CASP; every variant) | 198 | 12,672 | 0 |

Unicorn 2.1.4 (QEMU 5.0) compares `LDSMAX`/`LDSMIN` on bytes, halfwords and words as unsigned. `atomic.py` checks those against a Python model of the ARM pseudocode instead. The game does not use them.

## Jump tables

`tests/jumptables_check.py` runs `recomp/jumptables.py` on every `br` and compares it with LAS Recomp's Unicorn method. Functions are `[seed, next seed)` over the module's seeds plus `BL` targets (2026-10-20, `.venv`, 36 s wall for all three modules):

| Module | `br` | computed | resolved | entries | oracle resolved | agree | oracle wrong | computed, unresolved | PLT stubs (not computed) |
|---|---|---|---|---|---|---|---|---|---|
| `main` | 26,426 | 2,242 | 2,219 | 16,657 | 1,771 | 1,758 | 12 + 1 | 22 | 642 |
| `sdk` | 11,457 | 707 | 694 | 5,330 | 586 | 585 | 0 | 12 (+1 only the oracle resolves) | 8,933 |
| `subsdk0` | 4,465 | 339 | 339 | 2,529 | 312 | 312 | 0 | 0 | 3,925 |

- "Oracle wrong" carries independent evidence: the table base the oracle replays is not the constant every reaching definition gives (8 sites; the register is reused for other values, e.g. `x3` at `main+0x7c6fa8`), or the table index comes from a register or stack slot the oracle does not set (5 sites, e.g. `and w9, w1, #3` at `main+0x3d358`, a stack reload at `main+0xce2af4`).
- Every hoisted table base (482 in `main`, 86 in `sdk`, 30 in `subsdk0`) is confirmed by a capstone reaching-definitions pass.
- Left unresolved, by pattern: index `(x & 7) - 1` with no check (an unreachable default; 17 sites in `main`, one templated function family), index from an argument, a field or a stack slot without a visible check (`main+0x832f0`, `+0xcd7b40`, `+0xce2f38`, `+0x1386f24`, `sdk+0x10ec9c`, `+0x10f7e4`, `+0x10fd90`, `+0x11f2f0`), index from a bit insert of an unknown 32-bit value (`main+0x17d61c`), pointer difference in the `memchr`-style tails (`sdk+0xff1d0` and five more), a 64-bit index reloaded from the stack and compared as 32-bit (`sdk+0x32a518`, `sdk+0x591a4`), and a thunk that jumps to the next function through a constant (`sdk+0x56f4f4`).

## OS touch-points (MacNativeGames `patch_nso.py`, `DM_RULESET=native`)

Site kinds from `sites.tsv` (`x18` = platform register uses, `W` = exclusive windows, `J` = jump tables, `C` = call window):

| Module | x18 | tpidr_r | tpidrro_r | tpidr_w | cntpct | svc | W | J | C |
|---|---|---|---|---|---|---|---|---|---|
| `main` | 30,577 | 813 | | | | | 84 | 19 | 1 |
| `sdk` | 12,731 | 2 | 123 | 3 | 40 | 60 | 5 | 6 | |
| `subsdk0` | 10,201 | | | | | | | 2 | |

Rewriter statistics for `main`: 29,716 plain, 610 conditional, 232 PC-relative, 19 `br`, 813 TLS reads, 84 exclusive windows, 1 call window; 31,475 slot references. Runtime: `main` 77 s, `sdk` 12 s, `subsdk0` 7 s.

`patch_nso.py` prints Chinese text at the end of the `main` run; on a non-UTF-8 Windows console run it with `python -X utf8`.
