# S2: Headless Ghidra on the stripped ELF, and a usable function export

**Time box:** 1–2 sessions. **Depends on:** nothing. **Gates:** S3, S4, S6b (they copy this spike's project).
**Answers:** brief Q2, Q7 (VU0 micro-mode part), risk R2.

## Why

The boot ELF has no symbols. Everything downstream (the recompiler config, the overlay work, the runtime's
function tables) starts from a function map, and the only practical way to get one is Ghidra with the Emotion
Engine extension. PS2Recomp's exporter (`ps2xRecomp/tools/ghidra/ExportPS2Functions.java`) is written for the
GUI: it calls `askFile` twice and has no `getScriptArgs`, so headless use needs a `.properties` answer file.
This spike produces the first-party headless tooling under `tools/ghidra/` and the first function map.

## Inputs and tools

- `baserom/raw/SLUS_212.07`.
- Ghidra 12.1.4 at `DQ8_GHIDRA_INSTALL_DIR` (from `tools/local.env`) with `ghidra-emotionengine-reloaded`
  2.1.38 under `Ghidra/Extensions/` (language `r5900:LE:32:default`). Ghidra needs JDK ≥ 21; JDK 27 is on PATH
  and ran this install for the sibling MGS2 project. If it fails, install `jdk21-openjdk` and point
  `DQ8_JAVA_HOME` at it; `headless.sh` prefixes PATH, it never edits the shared install.
- The exporter: `work/ps2recomp/ps2xRecomp/tools/ghidra/ExportPS2Functions.java` (from the S1 clone, or
  download that one file into `work/ghidra/scripts/`).
- Pattern to copy: `MGS2_Undub/tools/native/run_headless.sh` (flock, XDG dirs and `-Dapplication.tempdir`
  under the workspace, `analyzeHeadless <projdir> <project> -scriptPath ...`).

## First-party scripts to write (committable, SPDX headers, `tools/ghidra/`)

| File | Purpose |
|---|---|
| `headless.sh` | Sources `tools/local.env`; `flock work/ghidra/headless.lock`; `XDG_CONFIG_HOME`/`XDG_CACHE_HOME`/tempdir under `work/ghidra/`; optional `DQ8_JAVA_HOME` PATH prefix; `GHIDRA_HEADLESS_MAXMEM=8G`; execs `support/analyzeHeadless "$1" "$2" -scriptPath "$PWD/tools/ghidra;$DQ8_EXTRA_SCRIPT_PATH" "${@:3}"`. |
| `Dq8AnalysisOptions.java` | preScript: turn off "Decompiler Parameter ID" (slow), turn on the EE "MIPS-R5900 Constant Reference Analyzer" if present, toggle "Aggressive Instruction Finder" from the script argument (`on`/`off`). |
| `InstructionCensus.java` | postScript: writes a CSV of mnemonic counts over all instructions; counts of `syscall` grouped by the `$v1` immediate set in the preceding `addiu`/`li`; `vcallms`/`vcallmsr`; COP2 macro ops; MMI ops; `cache`; `ei`/`di`; `mfc0`/`mtc0`; `sync`; plus function count, `FUN_`-named share, executable bytes covered by functions, and computed jumps (`jr` not `$ra`) without a resolved switch table. Counts only; no addresses of game data, no bytes. |
| `ExportPS2Functions.properties` | Answers the exporter's two `askFile` prompts for headless runs. Ghidra builds the property key from the `ask*` call's string arguments joined by spaces (check `docs/GhidraClass` or the `GhidraScript` javadoc in the install): e.g. `Choose output TOML config file Save = /abs/path/work/ghidra/export/main.toml`. Verify by test; the exporter writes nothing if a key is wrong. |

## Steps

```bash
set -a; . tools/local.env; set +a; mkdir -p work/ghidra/export
# 1. Import + plain analysis + census
tools/ghidra/headless.sh work/ghidra/base DQ8 -import baserom/raw/SLUS_212.07 -processor r5900:LE:32:default \
  -loader ElfLoader -max-cpu 8 -analysisTimeoutPerFile 7200 \
  -preScript Dq8AnalysisOptions.java off -postScript InstructionCensus.java work/ghidra/base/census-plain.csv \
  2>&1 | tee work/ghidra/import-plain.log
# 2. Same with the aggressive instruction finder, into a second project, for comparison
tools/ghidra/headless.sh work/ghidra/aggr DQ8 -import baserom/raw/SLUS_212.07 -processor r5900:LE:32:default \
  -loader ElfLoader -max-cpu 8 -analysisTimeoutPerFile 7200 \
  -preScript Dq8AnalysisOptions.java on -postScript InstructionCensus.java work/ghidra/aggr/census-aggr.csv \
  2>&1 | tee work/ghidra/import-aggr.log
# 3. Export with PS2Recomp's script (no re-analysis)
DQ8_EXTRA_SCRIPT_PATH=$PWD/work/ps2recomp/ps2xRecomp/tools/ghidra \
tools/ghidra/headless.sh work/ghidra/base DQ8 -process SLUS_212.07 -noanalysis -readOnly \
  -propertiesPath $PWD/tools/ghidra -postScript ExportPS2Functions.java 2>&1 | tee work/ghidra/export.log
head -3 work/ghidra/export/main.csv; wc -l work/ghidra/export/main.csv
```
Record wall time and heap per run, and **how the ELF loader treated the six overlapping zero-filesize
`PT_LOAD` segments at `0x00461C80`** (block names, conflicts, errors). If import fails on them, retry on a
scratch copy of the ELF with `e_phnum` reduced to the real segments (local edit under `work/`, never committed)
and record that this was needed. Ghidra can exit 0 after a script exception: grep the logs for `IMPORTED`,
`Wrote`, and `Exception`.

Leave `work/ghidra/base` **closed and unmodified** when done. S3/S4/S6b copy it (`cp -a work/ghidra/base work/ghidra/s3`)
because a Ghidra project cannot be opened by two headless processes.

## Success criteria

Extension and language load under the JDK in use; import completes; the exporter runs headless and writes
CSV (`Name,Start,End,Size`) and TOML; both census files exist; the segment verdict is recorded.

## Deliverables (findings)

- Function counts (plain vs aggressive), coverage %, `FUN_` share, computed-jump count; census highlights:
  syscall numbers used (first list of kernel services the runtime must HLE, architecture.md §5.1),
  `vcallms` count (does the game use VU0 micro mode? brief Q7), MMI and COP2 usage, `cache` sites.
- Exporter output counts (`[ghidra_export]`), the exact headless invocation, time and heap.
- The segment-handling verdict (feeds S3 and M2).
- A proposal for the function-map policy: the CSV holds addresses, sizes and Ghidra's auto-names
  (`FUN_xxxxxxxx`). The brief (§9) allows committing addresses and authored names but flags function maps
  for legal review; recommend whether `config/functions/SLUS_21207/main.csv` is committed now or kept local
  until that review.

## Must not commit

Ghidra projects (`*.gpr`, `*.rep/` are ignored), logs that contain disassembly, the export CSV/TOML until the
policy above is decided, any scratch-edited ELF.

## Findings

**Session:** 2026-10-19, about one session (roughly 50 minutes wall time, of which about 25 minutes were headless runs). **Stop reason:** done.

All numbers below come from `work/ghidra/*/census-*.csv` (written by `tools/ghidra/InstructionCensus.java`), the
headless logs under `work/ghidra/*.log`, and `work/ghidra/export/main.toml`. They are counts, addresses, sizes and
timings only. Nothing from the disc is reproduced here.

### Facts

**Toolchain.**
- Headless Ghidra 12.1.4 with `ghidra-emotionengine-reloaded` 2.1.38 (`extension.properties` reports `version=12.1.4`, its changelog "v2.1.38: Added support for Ghidra 12.1.4") loads `r5900:LE:32:default` under the OpenJDK 27 on PATH. Import, analysis, the census post-script and the exporter all exit 0 with no `Exception` in any log. JDK 27 prints two benign warnings at start-up (reflective mutation of a final field by gson, `sun.misc.Unsafe` use by Felix). Evidence: `work/ghidra/import-plain.log` lines 6–28, `grep -c -i exception work/ghidra/*.log` is 0 for every run.
- `tools/ghidra/headless.sh` keeps Ghidra's user state under `work/ghidra/` (`config/`, `cache/`, `tmp/`, `headless.lock`), defaults `GHIDRA_HEADLESS_MAXMEM` to 8G, prefixes `DQ8_JAVA_HOME` when set, puts `tools/ghidra` and `DQ8_EXTRA_SCRIPT_PATH` on the script path and runs from the repository root so relative paths in script arguments and `.properties` files resolve from there. The `LaunchSupport -save` step wrote its JDK choice under `work/ghidra/config/ghidra/`, so the shared install stayed untouched.
- `tools/ghidra/Dq8AnalysisOptions.java` found 132 registered analysis options for this language. Headless defaults already had `Decompiler Parameter ID = false`, `MIPS-R5900 Constant Reference Analyzer = true` and `Aggressive Instruction Finder = false`; the script confirms them and flips the finder from its argument. Dump of all option names and values: `work/ghidra/base/analysis-options.txt`.
- The EE extension registers two analyzers of its own on this language, `MIPS-R5900 Constant Reference Analyzer` and `MIPS-R5900 Unaligned Instruction Fix` (both on), plus `STABS` and `DVP Overlay Label Sync` (off, not applicable: no symbols, no VU overlay sections). It also ships 138 labelled function-start byte patterns for the EE kernel-call stubs (`data/patterns/r5900_LE_patterns.xml`), which is why 51 distinct SDK names (`SetGsCrt`, `CreateThread`, `WaitSema`, `sceSifSetDma`, ...) appear in the function list of a stripped ELF.
- **The exporter runs headless unmodified.** PS2Recomp's `ExportPS2Functions.java` (clone `work/ps2recomp` at commit `eb8688b2`, 2026-10-14) asks `askFile("Choose output TOML config file", "Save")` and `askFile("Choose output CSV file", "Save")`. Ghidra builds the property key from the trimmed title and button text joined by one space (`GhidraScript.join`, `Base-src.zip`), so `tools/ghidra/ExportPS2Functions.properties` holds `Choose output TOML config file Save = work/ghidra/export/main.toml` and `Choose output CSV file Save = work/ghidra/export/main.csv`. Evidence: `work/ghidra/export.log` line 59 "Reading script properties file: .../tools/ghidra/ExportPS2Functions.properties" followed by both "Exported ..." lines. Two details from the Ghidra source worth knowing: if the script is given any arguments, headless `askFile` takes its answers from them in order and ignores the file; and with neither, the script fails with "Error processing variable ... in headless mode" rather than writing nothing.
- A closed Ghidra project directory can be moved: `work/ghidra/base` was renamed to `work/ghidra/plain` and a read-only census ran on it afterwards (`work/ghidra/census-plain.log`). S3/S4/S6b can `cp -a` the project as planned.

**Timings and memory** (12-core host, `-max-cpu 8`, heap cap 8G never reached; peak RSS sampled every 5 s from `/proc`):

| Run | Command (after `tools/ghidra/headless.sh`) | Wall | Peak RSS | Log |
|---|---|---|---|---|
| plain import + analysis + census | `work/ghidra/base DQ8 -import baserom/raw/SLUS_212.07 -processor r5900:LE:32:default -loader ElfLoader -max-cpu 8 -analysisTimeoutPerFile 7200 -preScript Dq8AnalysisOptions.java off work/ghidra/base/analysis-options.txt -postScript InstructionCensus.java work/ghidra/base/census-plain.csv` (project since renamed `plain`) | 97 s | 2.74 GB | `import-plain.log` |
| same with the aggressive finder | as above with `work/ghidra/aggr`, `Dq8AnalysisOptions.java on`, `census-aggr.csv` | 309 s | 3.44 GB | `import-aggr.log` |
| **base** (bss and overlay window mapped, plain finder) | `work/ghidra/base DQ8 -import ... -preScript MapElfSegmentTails.java 0x3d2680=bss 0x461c80=overlay_window -preScript Dq8AnalysisOptions.java off work/ghidra/base/analysis-options.txt -postScript InstructionCensus.java work/ghidra/base/census-base.csv` | 173 s (a first attempt with one merged block took 447 s) | 3.65 GB | `import-base.log` |
| census only on an existing project | `<project> DQ8 -process SLUS_212.07 -noanalysis -readOnly -postScript InstructionCensus.java <csv>` | 15–31 s | 0.65–1.7 GB | `census-*.log` |
| exporter | `DQ8_EXTRA_SCRIPT_PATH=$PWD/work/ps2recomp/ps2xRecomp/tools/ghidra` ... `work/ghidra/base DQ8 -process SLUS_212.07 -noanalysis -readOnly -propertiesPath $PWD/tools/ghidra -postScript ExportPS2Functions.java` | 21–30 s | 0.87–0.97 GB | `export.log` |

Nothing hit `-analysisTimeoutPerFile 7200`; the two-hour figure in the brief was two orders of magnitude too cautious for this ELF.

**Segment verdict (the six zero-file-size `PT_LOAD` segments at `0x00461C80`).**
- `readelf` on the boot ELF: 8 program headers. Segment 0 is `RWE`, `vaddr 0x00100000`, `filesz 0x2D2680`, `memsz 0x361C80`. Segments 1–6 are `RWE` at `0x00461C80` with `filesz 0` and `memsz` `0x20F00`, `0x500`, `0x17B00`, `0x1CD00`, `0x27E00`, `0x5780` (one per overlay, as brief §6 says). Segment 7 is `RW` at `0x00489A80`, size 0. There are 14 section headers: `.shstrtab`, an empty `.strtab` and `.symtab`, one `PROGBITS` section for segment 0, six zero-size `PROGBITS` at `0x00461C80`, one zero-size at `0x00489A80`, `.comment` and `.reginfo`.
- **Ghidra's ELF loader imports without error and without conflict, but it silently maps none of the zero-file-size memory.** It created exactly one loaded block from the file, `SECTION4` (`0x00100000`–`0x003D267F`, 2,958,976 bytes, initialized, rwx; named after the section index because sections exist). The log mentions only "Skipping zero-length segment [7, ...]" and "Skipping section [SECTION5..11] with invalid size 0x0"; segments 1–6 are not mentioned at all. The reason is in `ElfProgramBuilder` (`Base-src.zip`): the uninitialized part of a `PT_LOAD` (`memsz > filesz`) is only materialised by `expandProgramHeaderBlocks`, which runs **only when the file has no section headers**. So neither the bss tail of segment 0 (`0x003D2680`–`0x00461C7F`, 587,264 bytes) nor the overlay window (`0x00461C80`–`0x00489A7F`, 163,328 bytes) exists in a default import. No `e_phnum` edit was needed or tried.
- Consequence, measured: in the default import 189 memory references point at unmapped addresses and Ghidra's constant-propagation analyzers create no references into bss at all (they only create references to valid memory). `tools/ghidra/MapElfSegmentTails.java` (preScript, reads the program-header table from Ghidra's `_elfProgramHeaders` block, merges overlapping tails, keeps adjacent ones apart, creates uninitialized rwx blocks) adds `bss` and `overlay_window`. With them: memory references 156,333 → 170,011, of which 13,334 into `bss` and **374 into `overlay_window`**; unmapped references 189 → 15; **120 function entries are created inside `overlay_window`** (one-byte bodies, no code to disassemble), which are the main program's call targets into whichever overlay is resident. Everything else in the census is identical between the two imports. Evidence: `block_references_to` and `block_function_entries` rows in `work/ghidra/plain/census-plain.csv` versus `work/ghidra/base/census-base.csv`; `work/ghidra/import-base.log` lines 81–89.
- The extension's own memory map adds uninitialized blocks for `registers.io`, `vu0.code`/`vu0.data`, `vu1.code`/`vu1.data`, `registers.gs`, `iop_ram`, `RAM(uncached)`, `RAM(uncached_and_accelerated)`, `scratchpad` and the two BIOS mirrors; the pspec's `rom0` block at `0x1FC00000` fails with "Failed to add language defined memory block due to conflict" (benign, it overlaps the mirrors). References into those blocks (97 to I/O registers, 29 to scratchpad, 13 to IOP RAM, 5 to VU0 code) are the first inventory of hardware the main program touches directly.
- **Verdict:** the ELF is well-formed and Ghidra needs no patched copy; the overlapping segments are harmless because they are ignored. `work/ghidra/base` carries the `bss` and `overlay_window` blocks and is the project S3/S4/S6b should copy; the 120 entries and 374 references into the window are S3's starting list. For M2, the recompiler sees only segment 0's file bytes; each overlay must be imported as its own program (payload at `0x00461CC0`, header at the window base) and the window treated as a separate code unit (architecture.md §4.3).

**Function discovery (brief Q2, risk R2).**

| Measure | plain (`work/ghidra/plain`) | aggressive (`work/ghidra/aggr`) | base (bss + window mapped) |
|---|---|---|---|
| functions | 9,598 | 9,606 | 9,718 (9,598 in the image + 120 in `overlay_window`) |
| `FUN_`-named | 9,461 (98.6%) | 9,467 | 9,581 |
| thunks | 69 | 69 | 69 |
| instructions | 640,430 | 648,518 | 640,430 |
| instruction bytes | 2,561,720 (86.6% of the 2,958,976-byte image) | 2,594,072 | 2,561,720 |
| image bytes inside function bodies | 2,534,920 (85.7%) | 2,535,312 | 2,534,920 |
| instructions outside any function | 6,700 (1.0%, 26,800 bytes) | 14,690 | 6,700 |
| `jr` returns / computed `jr` / computed without a jump table | 9,655 / 152 / 31 | 9,956 / 153 / 31 | same as plain |
| `jalr` / with a resolved target | 1,124 / 131 | 1,127 / 131 | same as plain |
| syscall sites | 78 | 167 | 78 |
| memory references | 156,333 | 157,861 | 170,011 |

- The 397,256 bytes of the image that are neither instructions nor function bodies are the data that shares the single `RWE` segment with the code (*inferred* from the counts; the ELF has no `.data`/`.rodata` split). So the main program is roughly 2.44 MB of code, 0.39 MB of initialized data and 0.57 MB of bss.
- Of the 137 functions not named `FUN_`, 69 are `thunk_FUN_*`, 51 distinct kernel-call stubs carry SDK names from the extension's patterns (53 functions; `AddIntcHandler` and `AddDmacHandler` match twice), and the rest are Ghidra's `entry` and similar.
- **Aggressive finder: not worth it.** It adds 8 functions and 8,088 instructions, doubles the loose instructions (14,690) and reports 89 extra "syscall sites" whose `$v1` values are implausible (`0x0`, `0x1`, `0xFC`, `0xFD`, `-0xFE`, `-0xFF`, ...): data decoded as code. The plain pass is the baseline; `work/ghidra/aggr` is kept only as evidence.
- Switch tables: 121 of the 152 computed `jr` sites have a resolved table (Decompiler Switch Analysis on). The 31 unresolved ones and the 993 `jalr` sites without a static target are the cases the runtime's dispatch fallback (architecture.md §4.6) must handle; the recompiler cannot see their targets.
- Delay-slot instructions: 140,136 (21.9%). `nop`: 47,059 (7.3%).

**Census highlights (plain pass; identical for base).**
- **Syscalls: 78 sites, 65 distinct `$v1` values**, every one found as an `li`/`addiu` immediate within four instructions of the `syscall`. Positive numbers (54): `0x2 0x4 0x6 0xD 0x10 0x11 0x12 0x13 0x14 0x15 0x16 0x17 0x20 0x21 0x22 0x24 0x25 0x29 0x2B 0x2F 0x30 0x32 0x33 0x37 0x3C 0x3D 0x3E 0x40 0x41 0x42 0x44 0x45 0x4A 0x4B 0x56 0x5A 0x5B 0x64 0x6B 0x70 0x71 0x73 0x74 0x76 0x77 0x78 0x79 0x7A 0x7B 0x7C 0x7F 0x80 0x82 0x83`. Negative numbers (11, the EE kernel's interrupt-context variants): `-0x1A -0x1B -0x1C -0x1D -0x2F -0x34 -0x38 -0x43 -0x46 -0x77 -0x78`. Repeats: `0x74` ×5, `0x5A` ×5, `0x5B` ×3, `0x10`, `0x12` and `-0x2F` ×2. By the public SDK numbering these are the GS CRT setup, exit and program load, VTLB handler, INTC/DMAC handler and enable/disable calls, thread create/delete/start/exit/terminate/priority/queue/id/status/sleep/wakeup/suspend, thread and heap setup, semaphores, cache flush, GS IMR, vsync flag, `SetSyscall`, the SIF DMA/register calls, OSD execution, Deci2 and memory size; the exact number-to-name table is M2 work and must be cross-checked against the 51 stub names Ghidra already assigned (`[general].stubs` in `work/ghidra/export/main.toml`): `AddDmacHandler AddIntcHandler ChangeThreadPriority CreateSema CreateThread Deci2Call DeleteSema DeleteThread DisableDmac DisableIntc EnableDmac EnableIntc EndOfHeap ExitDeleteThread FlushCache GetMemorySize GetOsdConfigParam GetThreadId GsGetIMR GsPutIMR iDisableDmac iDisableIntc iEnableDmac iEnableIntc InitTLB iPollSema isceSifSetDChain isceSifSetDma iSignalSema iWakeupThread PollSema ReferThreadStatus RemoveDmacHandler RemoveIntcHandler RotateThreadReadyQueue sceSifDmaStat sceSifGetReg sceSifSetDChain sceSifSetDma sceSifSetReg sceSifStopDma SetGsCrt SetOsdConfigParam SetVSyncFlag SignalSema SleepThread StartThread SuspendThread TerminateThread WaitSema WakeupThread`. This is the first list of kernel services the runtime must HLE (architecture.md §5.1).
- **VU0 micro mode is used, barely:** `vcallms` 2 sites, `vcallmsr` 0 (brief Q7, VU0 part). The runtime needs a VU0 microprogram path, but an interpreter will do.
- **COP2 (VU0 macro mode) is used heavily:** 1,339 COP2-opcode instructions, of which 1,234 are VU macro ops (the matrix families dominate: `vmulax` 92, `vmadday` 85, `vmaddw` 65, `vmaddaz` 63, `vsub` 110 across field masks, `vmul` 77, `vadd` 51, plus `vdiv` 25, `vsqrt` 22, `vrsqrt` 4, `vwaitq` 47, `vnop` 30) and the rest are moves (`qmtc2` 51, `qmfc2` 27, `cfc2` 21, `ctc2` 4). Plus `lqc2` 600 and `sqc2` 354. The arithmetic-fidelity helpers (architecture.md §4.4) have to cover this whole macro set.
- **MMI: 565 instructions**, 403 of them parallel SIMD ops (`paddh` 76, `pcpyld` 50, `psrlh` 50, `qfsrv` 42, `pextlb` 34, `pand` 30, `pnor` 29, `pcpyud` 23, `pextub` 16, `pmaxh`/`pminh` 16 each, `psubb` 15, `ppacb` 8, `pcgth` 6, ...) and the rest pipeline-1 multiply/divide and quadword moves (`mult1` 39, `mfhi1`/`mflo1`/`mthi1`/`mtlo1`, `moveq`/`clear` aliases). 128-bit `lq` 18,881 and `sq` 19,267 (6.0% of all instructions), so the 128-bit register model is on the hot path, not a corner case.
- **COP0 and privileged:** `cache` 17, `ei` 19, `di` 3, `mfc0` 29, `mtc0` 21, `sync` 85, and unexpectedly `eret` 3 and `tlbwi` 2 inside a user-mode ELF (the SDK's TLB or exception set-up, or a kernel patch; M2 must look at those sites before deciding what `patch_cop0` should do).

**Exporter output** (`work/ghidra/export/`, from `base`; the same run on `plain` is in `work/ghidra/export-plain/`):
- `main.csv`: header `Name,Start,End,Size` then 68,791 records (68,672 from `plain`). `[ghidra_export]`: `function_count = 9718`, `code_label_count = 59073`, `csv_record_count = 68791`, `stub_count = 53`, `untracked_stub_count = 0`, `uncategorized_count = 9665`, `runtime_call_name_count = 656`. The 53 stubs land in `[general].stubs` as `Name@0xADDRESS` selectors because their Ghidra names match PS2Recomp's embedded handler list.
- **The CSV is not a function list.** 59,073 of its records are "executable labels": every primary non-function symbol in executable memory (branch targets Ghidra labelled `LAB_*`, switch cases `caseD_*`, `switchD_*`, and synthetic `entry_*` records the script derives from the code), one row each with its own address and size. The 120 one-byte "functions" inside `overlay_window` are exported too (records with `Start >= 0x00461C80`, all `Size 1`); they must be filtered out before the CSV is fed to the recompiler, which has no bytes there.
- The TOML's `[general]` carries absolute machine paths (`input`, `output`, `ghidra_output`) and hard-codes `patch_syscalls = false`, `patch_cop0 = true`, `patch_cache = true`. Our config generation (architecture.md §3 step 4) must own that file; the exporter's TOML is a sample, not a deliverable. S1 should take the CSV shape and these flags into account.

### What changes in the docs
- project-brief.md §6: add rows for the function count (9,598 in the image, 85.7% of image bytes in function bodies, 1.0% loose instructions), the 78 syscall sites and 65 distinct numbers, VU0 micro mode (2 `vcallms` sites), COP2 macro use (1,234 ops), MMI (565) and 128-bit loads/stores (38,148); in the "Overlay window" row replace "loader behaviour pending spike S3" with the Ghidra fact that the window and bss are not mapped by default and need `MapElfSegmentTails.java`.
- project-brief.md §11 (M2 row) and §12 (R2): function discovery on the stripped ELF is good enough to start the recompile; the residual risk is the 31 unresolved switch tables and 993 indirect `jalr` sites, not undiscovered code.
- project-brief.md §14: add a decision **D9, function-map publication policy** (recommendation below).
- architecture.md §3: say that in developer mode the map is regenerated from the disc with `tools/ghidra/headless.sh` (about two minutes) until the policy in D9 lets it be committed; §4.2/§4.6: cite the `jalr`/computed-`jr` counts as the dispatch-fallback load; §5.1: insert the syscall number list and the 51 stub names as the initial HLE coverage list, and note the negative-number (interrupt-context) convention; §5.4: VU0 micro mode is used at two sites, so the VU0 path cannot be skipped but may stay an interpreter; §14: the open legal item stays, with the regeneration policy as the interim.
- tech-stack.md §7: Ghidra row: headless confirmed under JDK 27; list `tools/ghidra/headless.sh`, `Dq8AnalysisOptions.java`, `MapElfSegmentTails.java`, `InstructionCensus.java`, `ExportPS2Functions.properties`; the exporter emits labels as well as functions. §12: add the timings table above.
- Spike briefs S3, S4, S6b: copy `work/ghidra/base` (it has `bss` and `overlay_window`); S3 starts from the 120 `overlay_window` function entries and 374 references into it, and imports each `BIN/*.BIN` payload at `0x00461CC0` into the copy.

### Decision impact
- **D9 (new), function-map policy: keep the map local for now; do not add `config/functions/SLUS_21207/main.csv` in this milestone.** The brief (§9) allows committing addresses, sizes and names we authored, and architecture.md §14 flags the map for legal review before the first public release. Three practical reasons to wait beyond that review: the map is 98.6% Ghidra auto-names plus addresses and is regenerated from the user's disc in about two minutes with the tooling from this spike, so committing it buys nothing yet; it churns with every analysis change (mapping bss alone changed the record count, and the overlay units are still to come); and PS2Recomp's CSV, with 59,073 label rows, is not the shape of `config/functions/<profile>/<unit>.csv` in architecture.md §13.4. Proposal: commit the generator and a `sha256` of the generated CSV per Ghidra/extension version for reproducibility, and commit the map itself only once it carries authored content (names, overrides, overlay units) and the review is done; record this as ADR when accepted.
- D4 (PS2Recomp relationship): the exporter needs no patch for headless use; the properties file is enough. The patch-queue argument for D4 is unaffected.
- No change to D1–D3, D5–D8.

### Local artifacts (git-ignored)
- `work/ghidra/base/`: **the project to copy** (`DQ8.gpr`, program `/SLUS_212.07`), plain finder, `bss` and `overlay_window` blocks; `census-base.csv`, `analysis-options.txt`.
- `work/ghidra/plain/`: the default-loader import (no bss, no window), `census-plain.csv`. `work/ghidra/aggr/`: aggressive-finder import, `census-aggr.csv`. `work/ghidra/tails/`: first attempt whose script merged bss and window into one block (superseded, can be deleted).
- `work/ghidra/export/main.csv`, `main.toml`: exporter output from `base`; `work/ghidra/export-plain/`: from `plain`.
- `work/ghidra/*.log`: one log per run, each ending in an `END ... exit= wall= peak_rss_kb=` line.
- `work/ghidra/config`, `cache`, `tmp`, `headless.lock`: Ghidra user state; `work/ghidra/scripts/`: empty (the exporter came from the S1 clone).

### Open questions
- Which of the 120 `overlay_window` entries and 374 references are real calls into overlay code, and which overlay each belongs to → S3.
- Function discovery quality inside the overlays themselves, and whether the `MWo3` tables are jump tables the window code uses → S3.
- The 31 computed jumps without a table and the 993 indirect calls: how many resolve at run time through the dispatch fallback → M2.
- `eret`/`tlbwi` sites in a user-mode ELF: what they set up and whether `patch_cop0` must special-case them → M2.
- The number-to-name table for the 65 syscall numbers (public SDK numbering) and the semantics of the negative numbers for the HLE kernel → M2 (architecture.md §5.1).
- Whether a Function ID database or the EE extension's patterns can name more SDK library functions than the 51 kernel stubs (libc, libgraph, libsd client stubs) → M2; would raise the authored-name share of the map.
