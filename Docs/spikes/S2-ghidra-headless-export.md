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
