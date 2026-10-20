# S1: Does PS2Recomp build here, and what does a game-module build cost?

**Time box:** 2 sessions. **Depends on:** nothing. **Answers:** brief Q1/Q10, risks R9/R11, decisions D3/D4.

## Why

PS2Recomp is the recompiler we plan to build on (tech-stack.md §5). It is reported to be tested mainly with
MSVC. We need to know that it builds with GCC 16 and Clang 23 on Linux, how it handles our ELF (stripped, six
zero-filesize `PT_LOAD` segments at `0x00461C80`), and what a full recompile-and-compile cycle costs in time,
RAM and disk, because the game module is built on the user's machine (architecture.md §3, tech-stack.md §6).

## Inputs and tools

- `baserom/raw/SLUS_212.07`; network for the clone and for CMake FetchContent.
- GCC 16.2.1, Clang 23.1.1, CMake 4.4.3, Ninja 1.13.2, `/usr/bin/time -v`.
- PS2Recomp pinned at `eb8688b2fd191129f103a39543002c005b33409c` (main, 2026-10-14). The repo has **no
  submodules**; `ps2xRecomp/CMakeLists.txt` fetches ELFIO `Release_3.12`, toml11 `v4.4.0`, fmt `12.1.0`,
  libdwarf `v2.2.0`, rabbitizer `1.14.3` at configure time.

## Steps

```bash
set -a; . tools/local.env; set +a
git clone https://github.com/ran-j/PS2Recomp work/ps2recomp && git -C work/ps2recomp checkout eb8688b2fd191129f103a39543002c005b33409c
# 1. Build with both compilers (Release, SSE4.1 as the planned baseline), run the upstream tests.
for cc in gcc clang; do
  case $cc in gcc) CXX=g++;; clang) CXX=clang++;; esac
  CC=$cc CXX=$CXX cmake -S work/ps2recomp -B work/ps2recomp/build-$cc -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS=-msse4.1 -DCMAKE_CXX_FLAGS=-msse4.1 -DPS2X_BUILD_STUDIO=OFF 2>&1 | tee work/ps2recomp/configure-$cc.log
  /usr/bin/time -v ninja -C work/ps2recomp/build-$cc 2>&1 | tee work/ps2recomp/build-$cc.log
  work/ps2recomp/build-$cc/ps2xTest/ps2x_tests 2>&1 | tail -20 | tee work/ps2recomp/tests-$cc.log
done
```
Record every error and warning that needed a change; keep fixes as `git diff` files in `work/ps2recomp/patches/`
with a one-line reason each (they become `patches/ps2recomp/NNNN-*.patch` later). Note which targets exist
(`ps2_recomp`, `ps2_recomp_lib`, analyzer, runtime, IOP, tests) and their output paths.

```bash
# 2. Trial recompile WITHOUT Ghidra (analyzer fallback), to measure the pipeline, not to get a good map.
B=work/ps2recomp/build-gcc
$B/ps2xAnalyzer/ps2_analyzer baserom/raw/SLUS_212.07 work/ps2recomp/dq8-trial.toml 2>&1 | tee work/ps2recomp/analyzer.log
# Read the TOML it wrote; list every key and its default in the findings. Set output = generated/trial.
/usr/bin/time -v $B/ps2xRecomp/ps2_recomp work/ps2recomp/dq8-trial.toml 2>&1 | tee work/ps2recomp/recomp.log
find generated/trial -type f | wc -l; du -sh generated/trial; grep -c . generated/trial/*.cpp | tail -1
```
Record: function count found, files written, total bytes, wall time, peak RSS, and **how the six zero-filesize
`PT_LOAD` segments at `0x00461C80` were treated** (ignored, warned, crashed, emitted as empty). Repeat with
`single_file_output = true`, `low_memory_mode = true`, and `output_worker_threads` = 1 and 0, and record the
differences.

```bash
# 3. Compile the generated tree against the runtime headers at -O1 (a throwaway CMakeLists in work/).
#    Measure wall time, peak RSS (/usr/bin/time -v), disk, with -j12. Then once more at -O2 for comparison.
```
Do not try to link or run the result; linking needs runtime stubs that are M2 work.

## Success criteria

Both compilers build PS2Recomp and its tests pass (with at most a few small patches); the trial recompile
produces C++ that compiles; we have numbers for the user-facing build cost (R9, Q10).

## Deliverables (findings)

- Build times, RSS and patch list per compiler; FetchContent inventory and the consequence for offline,
  reproducible setup (`FETCHCONTENT_SOURCE_DIR_<NAME>` overrides to our own pinned submodules, or
  `FETCHCONTENT_FULLY_DISCONNECTED`).
- Trial-recompile metrics table and the full list of TOML keys with defaults.
- The segment-handling verdict (feeds architecture.md §4.3 and the M2 design of per-overlay synthetic ELFs).
- Recommendations: D3 (system compiler vs bundled toolchain, with numbers), D4 (pin + patch queue vs fork), the
  commit to pin, and the `-O` level split for cold vs hot units (tech-stack.md §6.2).

## Must not commit

`work/ps2recomp/` (the clone and logs with addresses are fine to keep locally), `generated/` (recompiled game
code), the trial TOML (it lists game function addresses; keep it local until S2 decides the function-map policy).

## Findings

**Session:** 2026-10-19, two sessions (the first ended on the usage limit with the full module compile 44 % done; the
second finished with sampled compiles). **Stop reason:** done, with the full-tree compile replaced by a partial run
plus a
controlled sample on the coordinator's instruction.

Machine: AMD Ryzen 5 5600 (6 cores, 12 threads), 31 GiB RAM, NVMe; GCC 16.2.1, Clang 23.1.1, CMake 4.4.3, Ninja 1.13.2.
Sibling spike sessions (headless Ghidra for S2, a 12-job paraLLEl-GS build for S5, PCSX2 for S7) shared the machine
during steps 1 to 3, so wall times are upper bounds and the job count is given with each number; CPU time (user+sys)
is quoted alongside because it is less sensitive to contention. Peak memory is the cgroup `memory.peak` of a transient
`systemd-run --scope` around each command (it also counts page cache charged to the build, so it is an upper bound);
the "PSS sum" column is the sampled sum of proportional set sizes of the live processes. GNU time is not installed;
the wrapper is `work/s1/measure.py` and every row is in `work/s1/metrics.tsv`.

### Facts

#### Build of PS2Recomp at `eb8688b` (2026-10-14)

- **Both compilers build every target unpatched.** No source or CMake change was needed; `work/ps2recomp/patches/` is
  empty — evidence: `work/ps2recomp/build-{gcc,clang}.log` (no `error:` lines), `work/s1/metrics.tsv`.
- Upstream's own CI already runs exactly this configuration (Ninja, Release, `-msse4.1`, Linux GCC and Clang, plus MSVC
  x64) — evidence: `work/ps2recomp/.github/workflows/build.yml`. The README's "currently tested mainly with MSVC" is
  stale.
- Build cost, all targets except Studio (`PS2X_BUILD_STUDIO=OFF`), `ninja -j12`:

  | Step | Wall | CPU (user+sys) | Peak PSS sum | cgroup peak | Disk |
  |---|---|---|---|---|---|
  | configure, GCC, first run (network fetch of 9 dependencies) | 204 s | 50 s | 0.2 GiB | 1.0 GiB | `_deps/` 1.2 GB |
  | build, GCC 16.2.1 | 80 s | 604 s | 3.7 GiB | 3.7 GiB | `build-gcc/` 1.5 GB incl. `_deps` |
  | configure, Clang, offline via source-dir overrides | 4.5 s | 4.6 s | 0.08 GiB | 0.07 GiB | |
  | build, Clang 23.1.1 | 60 s | 492 s | 1.7 GiB | 1.7 GiB | `build-clang/` 120 MB (dependency sources shared) |
  | `ps2x_tests`, either compiler | 3 s | 3 s | 0.15 GiB | 0.2 GiB | |

  Evidence: `work/s1/metrics.tsv` rows `configure-gcc`, `build-gcc-j12`, `configure-clang-offline`, `build-clang-j12`,
  `tests-gcc`, `tests-clang`. The GCC build overlapped with the S2 Ghidra import; the Clang build ran alone.
- Warnings: GCC 4 (all `-Wstringop-overflow` inside raylib); Clang 26 (18 `-Wswitch` and 1 tautological compare in
  `ps2xRuntime/include/runtime/gs/ps2_gs_memory.h`, 4 `-Wdeprecated-literal-operator` in toml11, 1 in raylib). None in
  `ps2xRecomp` or `ps2xAnalyzer` — evidence: `grep warning: work/ps2recomp/build-*.log`.
- Release builds turn on IPO/LTO for every first-party target (`ps2xRuntime/cmake/ReleaseMode.cmake`,
  `EnableFastReleaseMode`); on MSVC the same function forces `/arch:AVX2`. On GCC/Clang nothing adds `-march`, so
  `-msse4.1` from the command line is the only SIMD baseline; the runtime headers use SSE4.1 intrinsics (`smmintrin.h`)
  and nothing from AVX (`grep -E '_mm256|__AVX' ps2xRuntime/include` is empty).
- Targets and output paths (relative to a build dir): `ps2xRecomp/ps2_recomp` (2.3 MB) and `libps2_recomp_lib.a`;
  `ps2xAnalyzer/ps2_analyzer` (13.8 MB, embeds an SCE symbol database) and `libps2_analyzer_lib.a`;
  `ps2xRuntime/ps2EntryRunner` (38 MB) and `libps2_runtime.a`; `ps2xIOP/libps2_iop.a`; `ps2xTest/ps2x_tests` and
  three `ps2xTest/gs_cache/ps2_gs_*_cache_tests` binaries.
- Tests: `ps2x_tests` has 445 cases and **445 pass with both compilers when run from the clone root**. From any other
  working directory one case ("VU0 macro mappings cover all S1/S2 enums") fails, because it opens
  `ps2xRecomp/include/ps2recomp/instructions.h` by relative path (`ps2xTest/src/code_generator_tests.cpp` near line
  1271). The separate `gs_cache` CTest suites (45 tests, registered only under `ps2xTest/`, so run `ctest` from
  `build-*/ps2xTest`) pass 45/45 with both — evidence: `work/ps2recomp/tests-{gcc,clang}*.log`.
- `PS2X_ENABLE_SCCACHE` defaults to ON and silently uses `sccache` when it is on PATH (not installed here, so these are
  cold builds).

#### FetchContent inventory and offline builds

- The clone is 21 MB with no submodules. Configure fetches **nine** repositories, not five: `ps2xRecomp` fetches ELFIO
  `Release_3.12`, toml11 `v4.4.0`, fmt `12.1.0`, libdwarf `v2.2.0` and rabbitizer `1.14.3`; `ps2xAnalyzer` fetches
  nlohmann/json `v3.11.3`; `ps2xRuntime` fetches raylib `5.5`, Dear ImGui `v1.92.7-docking` and rlImGui `Raylib_5_5`.
  On Linux the runtime additionally needs system FFmpeg (`libavcodec`, `libavformat`, `libavutil`, `libswresample`,
  `libswscale` through pkg-config) unless `PS2X_ENABLE_FFMPEG=OFF`, and raylib needs X11 and GL development headers.
  ImGui and rlImGui are pulled with `FetchContent_Populate`, which CMake 4.4 flags as deprecated (CMP0169).
- Offline, reproducible configure works today without a patch: `FETCHCONTENT_FULLY_DISCONNECTED=ON` plus
  `FETCHCONTENT_SOURCE_DIR_<NAME>` for the nine names (`ELFIO`, `TOML11`, `FMT`, `LIBDWARF`, `RABBITIZER`,
  `NLOHMANN_JSON`, `RAYLIB`, `IMGUI`, `RLIMGUI`) configured the Clang tree in 4.5 s against the GCC tree's
  `_deps/*-src` checkouts — evidence: `work/ps2recomp/configure-clang.log`. In our tree those become pinned submodules
  under `third_party/` with rows in `THIRD_PARTY_NOTICES.md`; the recompiler and analyzer need only the first six, and
  the runtime trio (raylib, ImGui, rlImGui) plus FFmpeg fall away once we have our own runtime.
- The recompiler binary is ours to build and ship (GPL-3.0), so the 60 to 80 s PS2Recomp build is a developer and CI
  cost. The user pays for steps 2 and 3 below only.

#### Trial recompile of `SLUS_212.07` (analyzer fallback, no Ghidra map)

- `ps2_analyzer` on the boot ELF: **2.1 s wall, 2.1 s CPU, 0.3 GiB peak**. It found 8,314 functions with its JAL
  scanner, 0 symbols, 14 sections, 0 relocations, and matched 636 SCE SDK symbols from its embedded database (450
  renames, 186 additions). Its TOML (`work/ps2recomp/dq8-trial.toml`, 95 KB, kept local) lists 263 `stubs`, 372
  `entry_points`, 232 `[mmio]` entries, 5 jump tables of 46 to 89 entries, 9 instruction patches and 1,266
  `[performance].critical` names — evidence: `work/ps2recomp/analyzer.log`.
- `ps2_recomp` (GCC build) on that TOML, five output configurations:

  | Configuration | Wall | CPU | Peak PSS sum | cgroup peak | Files | Bytes | Lines of C++ |
  |---|---|---|---|---|---|---|---|
  | default (multi-file; `output_worker_threads` unset → 11 workers) | 1.7 s | 2.9 s | 360 MiB | 586 MiB | 8,335 | 201 MB | 4.77 M |
  | `single_file_output = true` | 1.5 s | 2.3 s | 236 MiB | 436 MiB | 4 | 199 MB | 4.68 M |
  | `low_memory_mode = true` (forces 1 worker) | 2.1 s | 2.1 s | 153 MiB | 402 MiB | 8,335 | 201 MB | 4.77 M |
  | `output_worker_threads = 1` | 2.1 s | 2.2 s | 171 MiB | 403 MiB | 8,335 | 201 MB | 4.77 M |
  | `output_worker_threads = 0` (the default, explicit) | 1.7 s | 2.9 s | 347 MiB | 565 MiB | 8,335 | 201 MB | 4.77 M |

  Evidence: `work/s1/metrics.tsv` rows `recomp-*`, logs `work/ps2recomp/recomp*.log`. The multi-file outputs are
  **byte-identical** across the three worker settings (`diff -rq`), so the generator is deterministic and a content
  keyed compiler cache will hit. Recompiling is not the cost; compiling the output is.
- Report (identical in all five runs): functions discovered 8,314; recompiled 8,117, stubs 197, skipped 0, decode
  failures 0; additional entry points 277,104; generated functions 8,332; indirect fallback promotions 1,028
  (459,720 fallback entries); unhandled instructions 7,948 in 422 functions (4,312 of them "SPECIAL"); 0
  correctness-critical failures; exit status 0. The unhandled-instruction errors cluster in a few hundred "functions" in
  the upper part of the address range, which is what decoding data as code looks like: the ELF's only non-empty
  section is one `PROGBITS` with flags `WAX` covering `0x00100000` to `0x003D2680` (code and data together), and the
  JAL scanner treats all of it as code. A Ghidra map (S2) replaces the scanner: with `ghidra_output` set, scanner
  functions that are not in the map are dropped (`elf_parser.cpp`, `extractFunctions`, "authoritative" ranges), so the
  map *is* the function-boundary policy.
- Output layout (multi-file): one `.cpp` per function named `<name>_0x<start>.cpp` (file names clamped to 100
  characters), plus `ps2_recompiled_functions.h` (8,342 lines of prototypes), `ps2_recompiled_stubs.h` and
  `register_functions.cpp`. The last is a flat dispatch table `g_ps2RecompiledFunctionTable[slotCount]` with one slot
  per
  4-byte address from the lowest to the highest registered function (739,742 slots for `0x00100008` to `0x003D2680`),
  filled by a static initializer: 285 k lines and 23 MB, the largest unit. The five largest per-function units are 2.9
  to
  7.2 MB (69 k to 188 k lines); the median unit is 6 KB. Single-file mode writes one 175 MB
  `ps2_recompiled_functions.cpp` plus the same `register_functions.cpp`.
- Generated code `#include`s, in this order: `<stdexcept>`, `ps2_runtime_macros.h`, `ps2_runtime.h`,
  `<ps2_recompiled_functions.h>`, `<ps2_recompiled_stubs.h>`, `ps2_syscalls.h`, `ps2_stubs.h` (and `ps2_log.h` under
  `_DEBUG`). Compiling it needs three include roots from the upstream tree: `ps2xRuntime/include`,
  `ps2xRuntime/src/lib/Kernel` (for `Stubs/Unimplemented.h`, pulled in by `ps2_stubs.h`) and `ps2xIOP/include`
  (`ps2x/iop/iop_types.h`). `ps2_runtime.h` includes neither raylib nor FFmpeg, so the generated tree compiles without
  them. A 6 KB unit preprocesses to 174 k lines, 5.4 MB (23 upstream and 537 system headers), which is where the
  per-unit compile time goes. 422 units contain `throw` (the emitter's fallback for the unhandled instructions above)
  and 25 contain
  `try`, so `-fno-exceptions` is not a drop-in flag for upstream output today.
- "No BSS found" in the report: the recompiler derives BSS from `SHT_NOBITS` sections, and this ELF expresses its
  0x8F600-byte BSS (`0x003D2680` to `0x00461C80`) only through the first segment's `memsz`. Harmless for the upstream
  runtime, whose `loadELF` zeroes `memsz - filesz` from the program headers; relevant for our own loader (M2).

#### TOML keys read by `ps2_recomp` (from `ps2xRecomp/src/lib/config_manager.cpp` at this commit)

| Key | Type | Default | Notes |
|---|---|---|---|
| `[general].input` | string | required | ELF path |
| `[general].output` | string | required | output directory, created if missing |
| `[general].ghidra_output` | string | `""` | CSV `Name,Start,End,Size` with a header line; hex or decimal; entries outside executable sections are dropped with a warning |
| `[general].single_file_output` | bool | `false` | one `ps2_recompiled_functions.cpp` instead of one file per function |
| `[general].low_memory_mode` | bool | `false` | serial output generation, no retained disassembly strings |
| `[general].output_worker_threads` | int | `0` | `0` = hardware threads − 1; clamped to `[0, 2 × hardware threads]`; legacy alias `output_worker_thread` |
| `[general].patch_syscalls` | bool | `false` | apply `[patches]` entries that hit `SYSCALL` |
| `[general].patch_cop0` | bool | `true` | apply patches that hit COP0 instructions |
| `[general].patch_cache` | bool | `true` | apply patches that hit `CACHE` |
| `[general].stubs` (or top-level `stubs`) | string[] | `[]` | `name` or `handler@0xADDR`; generic `ret0`, `ret1`, `reta0` |
| `[general].entry_points` (or top-level; legacy `untracked_stubs`) | string[] | `[]` | guest functions kept and exposed as exact entries |
| `[general].skip` (or top-level `skip`) | string[] | `[]` | not recompiled; emitted as `TODO_NAMED` wrappers |
| `[patches].instructions` | array of `{address, value}` | `[]` | raw instruction replacements; both fields string or integer |
| `[mmio]` | table `"0xINSTR" = "0xMMIO"` | `{}` | per-instruction MMIO address hints |
| `[jump_tables].table` | array of `{address, base_register, entries = [{index, target}]}` | `[]` | configured switch tables |
| `[performance].critical` | string[] | — | written by the analyzer, **ignored** by the recompiler |

There is no key for a code-unit name, a base address, an address window or an output prefix: the recompiler models
exactly one ELF with one flat address space per run.

#### How the six zero-filesize `PT_LOAD` segments at `0x00461C80` are treated

Shape of the ELF (shape only, from `readelf -lS`): 8 program headers and 14 section headers. Section 4 is the one
non-empty `PROGBITS` (`WAX`, `0x00100000`, size `0x2D2680`); sections 5 to 10 are six `PROGBITS` `WAX` sections at
`0x00461C80` with **size 0**; section 11 is a size-0 `WA` section at `0x00489A80`; segments 1 to 6 are the matching
`PT_LOAD`s with `filesz 0` and `memsz` `0x20F00`, `0x500`, `0x17B00`, `0x1CD00`, `0x27E00`, `0x5780`; segment 7 is
`0x00489A80` with `memsz 0`. `.symtab` and `.strtab` exist but are empty. There are no `.debug_*` sections. (This
corrects brief §6: the ELF is stripped, not section-less.)

- **Verdict: silently ignored. No warning, no crash, nothing emitted.** Because the ELF *has* section headers,
  `ElfParser::loadSections` takes the section path: all 14 headers become `Section` records (hence "14 sections" in
  every report), the seven empty ones with `size = 0` and `data = nullptr`. Every lookup is a half-open range test
  (`FindSectionByAddress`, `FindCodeSectionByAddress`), so a zero-size section never matches; the JAL scanner skips
  sections with `size < 4` or null data; the code generator never sees them. The segment-based fallback
  (`AppendLoadSegmentsAsSections`) runs only when there are **no** section headers; there a `filesz 0` segment yields no
  `LOADn` record and `memsz > filesz` yields a `LOADn.bss` record that is `isData`, never `isCode`. Either way the
  window
  is not code to the recompiler.
- Consequence, demonstrated: a two-row function-map CSV we authored for the probe (`work/ps2recomp/s1-probe-map.csv`:
  one entry at `0x00461C80`, one at `0x00100008`) run through `ps2_recomp` logs
  `[warning] ghidra-map - Ignored 1 Ghidra function(s) outside executable sections.` and generates only the main-window
  entry — evidence: `work/ps2recomp/recomp-probe.log`. **Overlay code can never be recompiled from the boot ELF**, with
  or without a map. Each overlay must reach the recompiler as its own ELF image whose section covers the window with
  real bytes (a synthetic ELF per `BIN/*.BIN` code unit, as architecture.md §4.3 plans). Each such run emits its own
  `register_functions.cpp` defining `g_ps2RecompiledFunctionTable*` over `0x00461C80`, so six units cannot be linked
  into one image unprefixed; selecting among them is the `OverlayManager`'s job, and the upstream runtime's single
  table keyed by absolute address cannot do it.
- Side effect in the upstream runtime's own loader: `PS2Runtime::loadELF` skips `memsz 0` segments and, for the six
  others, reads nothing and `memset`s `memsz` bytes to zero at `0x00461C80` (six times over the same window). Harmless.

#### Compiling the generated tree (`work/s1/module-build/`, an OBJECT library, nothing linked)

- **Full tree, GCC 16 `-O1`, `ninja -j12`, interrupted at 3,692 of 8,333 units.** The run overlapped with the S5
  paraLLEl-GS build (`-j12`), headless Ghidra (S2) and PCSX2 (S7); the coordinator reniced it to 19 part-way; the
  session's usage limit then stopped it. What it measured first is still informative — evidence:
  `work/s1/module-gcc-O1.log`, `work/s1/module-build/build-gcc-O1/.ninja_log`:

  | Partial run | Value |
  |---|---|
  | Units compiled | 3,695 of 8,333 (44 %), 90.9 MB of 200.5 MB of source (45 %), 0 errors |
  | Elapsed, first start to last finish | 24.4 min; 151 units/min on average (about 60/min during the worst contention) |
  | Sum of per-unit wall time | 17,150 s (effective parallelism 11.7 on 12 jobs) |
  | Per-unit wall | median 3.6 s, p90 5.3 s, p99 33 s, max 556 s |
  | Largest unit, `register_functions.cpp` (23 MB, the flat dispatch table) | 556 s on one core |
  | Next largest units that finished | 1.9 MB → 62 s, 1.8 MB → 128 s, 1.1 MB → 35 s, 1.0 MB → 46 s |
  | Peak memory seen | 4.2 GiB cgroup peak 7 % in, before the 23 MB unit; the run's own total was lost with the interrupt |
  | Objects on disk | 3,693 objects, 55 MB (build dir 80 MB) |

  The 7.2, 4.9, 4.1, 3.2 and 2.9 MB function units had **not** finished (the 7.2 MB one was in flight), so the tail of
  the distribution is only partly measured. **Estimate, clearly under contention:** a straight extrapolation of
  151 units/min gives about 55 min for the whole tree at `-j12`; scaling the per-unit sum by bytes gives about 38,000 s
  of single-core work, again about 55 min on 12 jobs; the single longest unit (556 s there, 204 s uncontended in the
  sample below) puts a floor of a few minutes on any job count. A median unit is 6 KB of C++ yet costs 3.6 s, so the
  bulk cost is header parsing, which the PCH sample below isolates.
- **Controlled sample, `nice -n 19 ninja`, 4 jobs (8 for the last three rows), with S3's Ghidra and, during the
  Clang row, S7's emulator as the other CPU users:** 417 units (every 20th by name,
  `work/s1/sample-every20.txt`), 7.1 MB of the 200.5 MB, none of the giants; plus the two largest units alone.

  | Build (`nice -n 19 ninja`, -j4 for the first three rows, -j8 after) | Units | Wall | CPU (user+sys) | CPU vs GCC -O1 | Peak PSS sum | cgroup peak | Per-unit wall median / p90 / max | Objects |
  |---|---|---|---|---|---|---|---|---|
  | GCC 16 `-O1` | 417 | 192 s | 676 s | 1.00× | 1.3 GiB | 1.3 GiB | 1.6 / 1.9 / 14 s | 4.7 MB |
  | GCC 16 `-O2` | 417 | 164 s | 648 s | 0.96× | 1.5 GiB | 1.5 GiB | 1.4 / 1.7 / 11 s | 5.0 MB |
  | Clang 23 `-O1` | 417 | 2838 s (paused 50 min) | 701 s | 1.04× | 0.6 GiB | 0.5 GiB | 1.6 / 2.0 / 2662 s (the paused unit) | 5.0 MB |
  | GCC 16 `-O1` + precompiled runtime headers | 417 (+1 PCH step) | 37 s | 252 s | 0.37× | 1.1 GiB | 1.2 GiB | 0.5 / 0.9 / 11 s | 4.7 MB |
  | GCC `-O1`, `register_functions.cpp` alone (23 MB) | 1 | 122 s | 122 s | — | 1.9 GiB | 1.9 GiB | 122.5 / 122.5 / 123 s | 9.8 MB |
  | GCC `-O1`, largest function unit alone (7.2 MB) | 1 | 205 s | 204 s | — | 3.0 GiB | 3.0 GiB | 205.0 / 205.0 / 205 s | 4.6 MB |

  Evidence: `work/s1/metrics.tsv` rows `sample-*` and `giant*`, logs `work/s1/sample-*.log`, `work/s1/giant*.log`,
  per-unit times in each build dir's `.ninja_log`. Caveats: the coordinator paused the Clang run with SIGSTOP for about
  50 minutes while S7 ran PCSX2 (hence its wall time and its one 2,662 s unit), and the last three rows ran at eight
  jobs
  instead of four, so **compare the CPU column**, not wall time. Readings:
  - A typical unit costs **1.6 CPU-s with GCC and 1.7 with Clang at either `-O1` or `-O2`**; the optimisation level is
    noise for the 99 % of units under 100 KB because the front end dominates (174 k preprocessed lines per unit).
  - A **precompiled header of the four runtime headers cuts a typical unit to 0.6 CPU-s (0.37×)**, the single largest
    saving available without touching the emitter.
  - The giants are superlinear and memory-hungry: the 23 MB dispatch table takes 122 CPU-s and 2.0 GiB alone, the
    7.2 MB single function 204 CPU-s and 3.0 GiB. Typical units need about 0.35 GiB per job with GCC and 0.15 GiB with
    Clang (peak PSS at four jobs divided by four).
  - **Whole-tree estimate (uncontended, derived):** 8,327 ordinary units × 1.6 CPU-s (14,000 to 15,000 CPU-s, scaling by
    units or by the 155 MB they hold) plus about 760 CPU-s for the six units over 2.9 MB gives **15,000 to 16,000 CPU-s
    without a PCH and about 6,000 with one**. On this 6-core machine at 12 jobs that is roughly **30 to 40 min, or about
    15 min with the PCH**; the measured partial run was heading for about 55 min under contention. The longest single
    unit (204 s) is the floor at any job count. Peak RAM to plan for: 0.4 GiB per job plus 3 GiB for the largest unit,
    so about 7 GiB at 12 jobs, 5 GiB at 6. Disk: about 125 MB of `-O1` objects for 201 MB of source, plus the source.

### What changes in the docs

- project-brief.md §6: the boot ELF has 14 section headers (one non-empty `WAX` `PROGBITS` plus seven empty ones),
  not none; the symbol tables exist but are empty; BSS extent `0x003D2680` to `0x00461C80` (from the first segment's
  `memsz`).
- project-brief.md §11 and §12 (R9): replace the borrowed "20 to 40 minutes, 20 GB" estimate with the numbers above:
  recompiling takes 2 s and 0.6 GiB; compiling the 8,333 units of the heuristic map is about 15,000 to 16,000 CPU-s
  (1.6 CPU-s per typical unit plus the giant units), or about 6,000 CPU-s with a precompiled header: roughly 30 to 40
  min of wall time on this 6-core machine, about 15 min with the PCH, and heading for 55 min under the contention we
  had; 5 to 7 GiB of RAM at 12 jobs; about 125 MB of objects.
- project-brief.md §12 (R11): upstream has 169 commits since 2025-04-12, 16 in the last 90 days from 7 authors, 5 tags;
  14 files under `ps2xRecomp`/`ps2xAnalyzer` changed in the last 30 days. "Moves quickly" holds; "unstable" is not what
  the build shows.
- architecture.md §4.3: record the verdict above; per-overlay synthetic ELFs are required, not optional, and each unit's
  `register_functions.cpp` must be renamed or wrapped per unit (otherwise every unit defines the same
  `g_ps2RecompiledFunctionTable*` symbols).
- architecture.md §3 (stages 5 and 6) and tech-stack.md §6.1: "about 6,400 files" becomes 8,335 files and 201 MB of C++
  for the boot ELF alone under the heuristic map; expect fewer and cleaner functions with the S2 map, plus six overlay
  units.
- tech-stack.md §5: the recompiler row should list all nine FetchContent dependencies (add nlohmann/json, raylib, ImGui,
  rlImGui) and that `ps2xRuntime` wants system FFmpeg on Linux; note that only six matter for the tools we keep.
- tech-stack.md §6.2: the generated code uses exceptions on its error path at this commit, so "no exceptions, no RTTI"
  needs a clean map or an emitter patch; add `ps2xRuntime/src/lib/Kernel` to the module's include roots; a precompiled
  header of the four runtime headers changes the typical unit from 1.6 CPU-s to 0.6 CPU-s (0.37×), so the PCH row in
  §6.2 is confirmed and should be the default.
- tech-stack.md §12: the README's "tested mainly with MSVC" is stale; upstream CI builds Linux GCC and Clang.

### Decision impact

- **D3 (how the game module is produced):** keep **option A (system compiler) as the Linux default**; the data
  supports it. Both system compilers compile the recompiler's output unpatched; the module needs nothing beyond a
  C++20 compiler, CMake and Ninja (the upstream runtime headers that the generated code includes are self-contained,
  no raylib, FFmpeg or other library is needed at this stage, and our own headers will be too). The cost that matters
  for the first-run experience is not the toolchain but the compile itself: 30 to 40 min without a precompiled header
  and about 15 min with one on six cores, 5 to 7 GiB at 12 jobs (about 0.35 GiB per job for typical units, 2.0 and 3.0
  GiB for the two largest units), so the launcher must pick the job count from free RAM, show progress, and use a
  compiler cache keyed on the deterministic output. Windows (option B, bundled toolchain) stays unmeasured by this
  spike; the recommendation there is unchanged and should be validated on a Windows runner before M5.
- **D4 (relationship to PS2Recomp):** recommend **pin `eb8688b2fd191129f103a39543002c005b33409c` plus a patch queue, not
  a fork**, with the queue starting empty: zero patches were needed to build and run on GCC 16 and Clang 23. The pieces
  we will certainly change (one address space per run, the flat dispatch table in `register_functions.cpp`, `throw` on
  unhandled instructions, no per-unit prefix) are small and local to `ps2xRecomp`, and upstream treats Linux CI as first
  class, so they are candidates to send upstream. Keep the nine FetchContent names as pinned submodules and configure
  with `FETCHCONTENT_FULLY_DISCONNECTED=ON` (verified). Re-evaluate at M2 if the per-code-unit changes grow past a few
  hundred lines.
- **`-O` split (tech-stack.md §6.2):** on typical units `-O2` costs the same CPU as `-O1` (0.96× on the 417-unit
  sample), so an `-O1`/`-O2` split buys nothing for the 99 % of units under 100 KB; recommend **`-O2` for all function
  units** and **`-O0` or `-O1` for `register_functions.cpp`**, which is a 23 MB static-initializer table with nothing
  to optimise (122 CPU-s and 2.0 GiB at `-O1`). The remaining lever is the handful of multi-megabyte single-function
  units (204 CPU-s and 3.0 GiB for the 7.2 MB one at `-O1`): they set the critical path at any job count, so the
  emitter should split them (or the map should) before we tune flags further.
- **R9 (build time and disk for users):** measured, not borrowed: recompile 2 s and 0.6 GiB; compile 15,000 to 16,000
  CPU-s, about 6,000 with a precompiled header (30 to 40 min of wall time on six cores, about 15 min with the PCH,
  which cuts a typical unit to 0.37×); RAM 5 to 7 GiB at 12 jobs; disk about 0.5 GB (201 MB of generated source, about
  125 MB of objects, 21 MB clone), not 20 GB. Keep R9 at Medium for time and lower the disk concern; the mitigations
  already listed (PCH, compiler cache, chunking of the giant units, job count from RAM) are the right ones and each is
  now backed by a number.

### Local artifacts (git-ignored)

- `work/ps2recomp/`: the clone at `eb8688b`, `build-gcc/` (1.5 GB, includes the 1.2 GB `_deps/` sources) and
  `build-clang/` (120 MB, configured against the GCC `_deps` sources); `patches/` (empty).
- `work/ps2recomp/configure-{gcc,clang}.log`, `build-{gcc,clang}.log`, `tests-{gcc,clang}.log`,
  `tests-gcc-from-clone-root.log`, `tests-{gcc,clang}-gs_cache.log`: step 1.
- `work/ps2recomp/analyzer.log`, `dq8-trial.toml`, `trial-{default,single,lowmem,w1,w0,probe}.toml`, `recomp.log`,
  `recomp-{single,lowmem,w1,w0,probe}.log`, `s1-probe-map.csv`: step 2. The TOML and the logs contain game addresses
  and stay local.
- `generated/{default,single,lowmem,w1,w0}/`: recompiler output, about 1 GB in total; delete once S2 supersedes it.
- `work/s1/measure.py` (timing and peak-memory wrapper), `work/s1/metrics.tsv` (every measurement in this section),
  `work/s1/run-samples.sh`, `run-samples2.sh`, `sample-table.py` and its output `sample-table.md`.
- `work/s1/module-build/`: the throwaway CMake project for step 3; `build-gcc-O1/` (the partial full-tree run, 80 MB),
  `sample-*/`, `giant-gcc-O1/`, `giant2-gcc-O1/`; `work/s1/sample-every20.txt`, `giant-register.txt`,
  `giant2-function.txt`; logs `work/s1/module-gcc-O1.log` (and `module-gcc-O1-attempt1-badinclude.log`, the first
  attempt that lacked the Kernel include root), `work/s1/sample-*.log`, `work/s1/giant*.log`.

### Open questions

- How many real functions does the boot ELF have, and how many of the 8,117 heuristic ones are data? → S2 (Ghidra map).
- What are each overlay's function count and generated size, and does a per-overlay synthetic ELF round-trip through the
  recompiler? → S3 for the load addresses and sizes, then an M2 prototype.
- Is a flat table with one slot per 4 bytes over the whole code range acceptable for six units sharing a window, or do
  we generate a per-unit sparse table? → M2 design, after S3.
- Does the compile cost scale with the map (fewer, larger functions), and can the emitter split the multi-megabyte
  single-function units? → re-measure once S2 provides the map.
- Windows numbers (MSVC or llvm-mingw) for D3 → a later spike on a Windows machine or a CI runner.
- A clean, uncontended full-tree compile for the record → rerun `work/s1/module-build` at `-j12` when the machine is
  free (about an hour).
