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
