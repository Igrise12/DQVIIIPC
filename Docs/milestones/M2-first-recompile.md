# M2: First recompile

| | |
|---|---|
| **Status** | **Done 2026-10-20.** Exit criterion met on the real disc (see Results). |
| **Exit criterion** (project-brief.md §11) | Function maps for the main ELF and all six overlays; the recompiler's C++ compiles and links; the program starts from `0x00100008` and reaches the first unimplemented syscall |
| **Decisions it rests on** | ADR-0003 (PS2Recomp `eb8688b` pinned, own runtime), ADR-0008 (module built locally), architecture.md §2, §4, §13 |
| **Contract files (day 0, owned by the coordinator)** | `include/dq8/abi/game_module.h`, `include/dq8/abi/ps2recomp/ps2_runtime.h`, `r5900_context.h`, `ps2_address.h` |

This page is the shared brief for the five M2 sessions. Each session reads it, does its part in its own worktree,
and reports back; the coordinator merges in the order A → C → B → E → D and then runs the real pipeline.
Changes to a contract file go through the coordinator.

## What the recompiler gives us (PS2Recomp `eb8688b`)

- Every generated function is `void name(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime)`. Each
  instruction stores `ctx->pc` first. Names are `<name>_0x<start>`; Ghidra auto-names become `FUN_xxxxxxxx_0x...`.
- **Every `jal` goes through `runtime->dispatchGuestBranch(..., DirectCall, "JAL")`**; only a `j` to a function of
  the same unit is a direct C++ call. `jr`/`jalr` (not `$ra`) dispatch as `IndirectJump`/`IndirectCall`; `jr $ra` is
  `ctx->pc = ra; return;`. Calls nest on the host stack.
- **Dispatch contract** (upstream `ps2xRuntime/src/lib/ps2_runtime.cpp:1345-1412`): set `ctx->pc = target`; for a
  jump return `false` (the caller returns, the run loop continues at `ctx->pc`); for a call look the target up, run
  it, treat "pc unchanged" as returned, and return `ctx->pc == fallthrough`. When a call returns `false`, the
  generated caller returns too and the run loop re-enters through the **resume entry** at `jal+8`: a table slot that
  points at the owning function, whose `switch (ctx->pc)` jumps to the label. Resume entries exist for `jal+8`,
  `jalr+8`, `syscall+4` and backward-branch targets, **not** for `bgezal`/`bltzal`/`bal` returns (add those addresses
  as `entry_points`).
- Backward branches call `runtime->eeCheckpointDue()` (M2: always `false`). Syscalls emit `ctx->pc = next;
  runtime->handleSyscall(rdram, ctx, encoded)` and continue inline; the id is `(raw >> 6) & 0xFFFFF` (0 for SDK
  code, so the number is in `$v1`). Unhandled instructions are inline `throw std::runtime_error(...)`.
- Each unit's `register_functions.cpp` defines `g_ps2RecompiledFunctionTable{,Base,End,SlotCount}` (external
  linkage) and fills one slot per 4 bytes from the lowest to the highest entry with a static initialiser. Function
  symbols are global and identical across units with the same addresses (all six overlays start at `0x00461C80`).
- Inputs: one ELF per unit with an `SHF_EXECINSTR` section (section headers win over program headers;
  `e_machine == EM_MIPS`; `e_entry` 0 or a mapped function); a CSV `Name,Start,End,Size` (header line skipped, End
  **exclusive**, `0x` addresses; `sub_`/`FUN_`/`LAB_`/`DAT_` names are non-authoritative); a TOML with `[general]`
  `input`, `output`, `ghidra_output`, `patch_syscalls`, `patch_cop0`, `patch_cache`, `stubs` (`Name@0xADDR` binds a
  function start to `ps2_syscalls::Name` or `ps2_stubs::Name`), `entry_points`, `skip`, and `[jump_tables]` keyed
  by the **table's data address** (`[[jump_tables.table]] address, base_register, entries = [{index, target}]`).
  Functions whose map name equals a `PS2_STUB_LIST` name are stubbed automatically; kernel-call names need
  `Name@0xADDR`. `ps2_recomp <config.toml>` exits 0 on success, 1 on failure.
- Known hazards: the constant-address hint path emits `FAST_READn/FAST_WRITEn(<literal>)` with no special-address
  test (our `FAST_*` must route); upstream's special predicate misses VU0 code and VU1 data (ours covers the whole
  VU range, `ps2_address.h`); `ps2_runtime.h` upstream includes its whole GS/VU1/audio/pad/VFS stack (we never use it).

## Design

### Module ABI (`include/dq8/abi/`, installed with the host)
- `game_module.h`: `dq8_game_module_desc`, `dq8_runtime_api`, the export `dq8_game_module_describe`, ABI version 1.
- `ps2recomp/` is an include root with the names the emitter includes. Day 0 provides `ps2_runtime.h` (the
  `PS2Runtime` adapter: `{const dq8_runtime_api* api; void* host;}` with inline forwards), `r5900_context.h`
  (`R5900Context`, `PS2Exception`, `getRegU32`; adapted from upstream, GPL-3.0-only) and `ps2_address.h` (memory map,
  `ps2_is_special_address`). Session A adds `ps2_runtime_macros.h`, `ps2_call_list.h`, `ps2_syscalls.h`,
  `ps2_stubs.h`, `ps2_log.h`.
- Modules have no undefined references to the host (`-Wl,-z,defs`), so the module compiler need not match the host's.
- **M2 deviation from architecture §13.3 (documented, temporary):** C++ exceptions cross the module boundary
  (`GuestStop` thrown by host hooks through module frames; the emitter's inline `throw`). Host and modules must share
  the C++ runtime (no `-static-libstdc++`; MSVC `/EHs /MD`). M3 replaces `GuestStop` with fiber switches; a patch-queue
  change to the emitter removes the inline throws.

### Per-unit module build (`cmake/module/`, installed with the host)
Standalone CMake project, one build tree per unit, parameters as cache variables: `DQ8_UNIT_NAME`, `DQ8_UNIT_KIND`
(`main`|`overlay`), `DQ8_UNIT_OVERLAY_ID`, `DQ8_UNIT_GENERATED_DIR`, `DQ8_UNIT_PROFILE_ID`, `DQ8_UNIT_SOURCE_SHA1`,
`DQ8_GENERATOR`, `DQ8_UNIT_ENTRY_POINT`, `DQ8_ABI_INCLUDE_DIR`, `DQ8_MODULE_PCH` (ON), `DQ8_MODULE_DEBUG_INFO` (OFF).
Output `dq8_unit_<name>` (shared): C++20, hidden visibility, `-O2 -msse4.1 -fexceptions -fno-rtti -w` for generated
sources, `register_functions.cpp` at `-O0`, per-target PCH (the ABI headers plus the unit's
`ps2_recompiled_functions.h` and `ps2_recompiled_stubs.h`), the four table globals renamed per unit with `-D`
(`dq8_unit_<name>_table`, `_table_base`, `_table_end`, `_table_slot_count`), `-Wl,-z,defs -Wl,--exclude-libs,ALL`,
`dq8_unit_entry.cpp.in` filling and exporting the descriptor. Include path: the unit's generated directory,
`<abi>/dq8/abi/ps2recomp`, `<abi>`.

### Host runtime (`src/runtime`, `src/game`, `src/host`)
- `dq8_runtime` (links `dq8::core`): `ee/guest_memory` (32 MiB RDRAM, 16 KiB scratchpad, host load/store for special
  addresses; MMIO log in M2: loads return 0, stores dropped, first hit per address logged, `--trap-mmio` stops),
  `ee/elf_loader` (via `core::parse_elf_headers`: copy `filesz`, zero `memsz - filesz` including the six window
  segments), `ee/context` (`pc = entry`, `sp = 0x01FFFFF0`, `a0 = a1 = ra = 0`; dump), `ee/dispatch_table`,
  `ee/overlay_manager` (window from the region profile; register units; `set_resident`; generation counter;
  `describe_miss`), `ee/dispatcher` (main table, else window → resident table, else miss), `ee/ee_runtime` (the
  `dq8_runtime_api` trampolines, `dispatch_guest_branch`, `run()` mirroring upstream `EeScheduler::run` with one
  thread; `ee_checkpoint_due` false; `handle_syscall` logs number, name, `$a0..$a3` and throws `GuestStop`),
  `ee/game_module` (load, `validate_descriptor` reporting every problem), `kernel/kernel_calls` (public SDK syscall
  number ↔ name), `kernel/kernel_hle` (empty in M2), `platform/dynlib` (`dlopen(RTLD_NOW | RTLD_LOCAL)` /
  `LoadLibraryW`).
- `dq8_game`: region profile → `OverlayWindowSpec` and the seven unit specs. The runtime never sees a DQ8 literal.
- `dq8-game --iso <path> [--regions DIR] --module-dir <dir> [--stop-at-first-syscall] [--max-dispatches N]
  [--trace-dispatch] [--trap-mmio] [--json]`; developer mode `--elf <file> [--window BASE:END] [--resident ID]`.
  Exit 0: stopped at a syscall as requested; 1: any other guest stop; 2: usage, I/O or module validation.

### Setup pipeline in C++ (`libdq8core`, `dq8ctl`)
- `core/elf/elf_writer` (ELF32 LE `ET_EXEC` `EM_MIPS`; sections and matching PT_LOADs) and
  `core/overlay/overlay_elf` (`make_overlay_unit_elf`: one `SHF_ALLOC|SHF_EXECINSTR|SHF_WRITE` PROGBITS covering
  header+text+data at the load address, NOBITS `.bss`, `e_entry = 0`, boot `e_flags`, no symbols).
- `core/platform/process` (portable subprocess runner).
- `core/recomp/`: `function_map` (read and validate CSV and jump-table fragment), `recomp_config` (one TOML per unit
  with toml11), `recomp_driver` (run `ps2_recomp`, parse report counts), `module_build` (configure/build
  `cmake/module` per unit, job count from free RAM, `build-manifest.toml` with input hashes, generator, timings).
- `dq8ctl recomp prepare|generate|build|all <iso> --maps <dir> --work <dir> [--units LIST] [--jobs N]
  [--ps2-recomp PATH] [--json]` (idempotent, resumable) and `dq8ctl make-unit-elf <bin> --out <file>`.
- Contributor tool: `tools/ghidra/ExportFunctionMap.java` + `tools/ghidra/export_maps.sh` (Ghidra, local).
- **Stays local:** `work/recomp/**`, `generated/**`, maps, TOMLs, modules. **Committed:** code, templates, scripts.
  Maps are contributor input for now (D9); before M5 the end-user path needs a map source that does not need Ghidra.

### Recompiler build and CI
- Submodules (day 0): `third_party/PS2Recomp` (`eb8688b`), `ELFIO` (`Release_3.12`), `fmt` (`12.1.0`),
  `libdwarf-code` (`v2.2.0`), `rabbitizer` (`1.14.3`); toml11 `v4.4.0` already present.
- `cmake/Ps2Recomp.cmake`: `ExternalProject_Add` on a copy of the PS2Recomp root (its `ps2xRecomp` includes
  `${CMAKE_SOURCE_DIR}/ps2xRuntime/cmake/ReleaseMode.cmake`), patch queue from `patches/ps2recomp/` (empty),
  `FETCHCONTENT_FULLY_DISCONNECTED=ON` plus `FETCHCONTENT_SOURCE_DIR_{ELFIO,TOML11,FMT,LIBDWARF,RABBITIZER}`,
  `PS2X_BUILD_{RUNTIME,ANALYZER,TEST,STUDIO}=OFF`, `PS2X_ENABLE_SCCACHE=OFF`, `-msse4.1`; option `DQ8_BUILD_RECOMP`.
- `e2e` (Linux, `DQ8_BUILD_E2E=ON`): synthetic MIPS fixtures, never game data; new CI job `recomp-e2e`.

## Sessions

Rules for every session: work in your own worktree branch and commit there in small steps; do not push; do not
edit files another session owns or the contract files (ask the coordinator); every source file gets an SPDX
header (`GPL-3.0-or-later`; files adapted from PS2Recomp get `GPL-3.0-only` plus an attribution line); format with
`.clang-format`; keep `ci-gcc`/`ci-clang` warning-free (MSVC portability: no POSIX-only calls outside
`#ifdef`); never commit game-derived bytes, generated code, maps or TOMLs with game addresses; never write the
literal marker strings the checker's `generated-code` rule refuses (see `scripts/check_no_game_data.py`);
before reporting run `python3 -I scripts/check_no_game_data.py --tree`, `pre-commit run --all-files`,
`reuse lint`, the unit tests, and merge `main` into your branch.

### A: ABI headers and module template
**Owns:** `include/dq8/abi/ps2recomp/{ps2_runtime_macros.h, ps2_call_list.h, ps2_syscalls.h, ps2_stubs.h, ps2_log.h}`,
`cmake/module/**`, `tests/abi/**` (parity test, sample unit), `include/README.md`.
- `ps2_runtime_macros.h`: adapted from `third_party/PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h` (GPL-3.0-only,
  attribution). Include our `ps2_runtime.h` instead of upstream's. Provide every macro family the emitter uses
  (`READ*`/`WRITE*`, `FAST_*` **routed through `PS2Runtime::isSpecialAddress`**, `GPR_*`/`SET_GPR_*`, integer ops,
  `FPU_*` with upstream's PS2 float semantics, MMI `PS2_P*`, VU `PS2_V*`, helpers `Ps2SignExt32ToU64`,
  `Ps2HiLoToU64`, `ps2_paddu32`, `ps2_psubu32`, `ps2_plzcw32`, `ps2_clz32`, `PS2_EXTRACT_*`, `PS2_BLENDV_PS`, ...).
  Check the list against `grep -oh` over a local `generated/` tree (identifiers only).
- `ps2_call_list.h`: byte-for-byte copy of upstream's; CTest `abi.call_list_parity` fails when it differs from the
  submodule's.
- `ps2_syscalls.h` / `ps2_stubs.h`: X-macro inline forwarders `name(rdram, ctx, runtime)` →
  `runtime->api->call_named(runtime->host, DQ8_CALL_SYSCALL|DQ8_CALL_STUB, "name", rdram, ctx)`; `TODO()` and
  `TODO_NAMED(const char*, ...)` declared inside the namespace (→ `DQ8_CALL_TODO`). `ps2_log.h`: no-op `PS_LOG_ENTRY`.
- `cmake/module/CMakeLists.txt` and `dq8_unit_entry.cpp.in` as specified above; MSVC equivalents (`/EHs /MD /GR-`).
- **Done when:** the headers compile standalone (GCC, Clang; MSVC in CI); a hand-written sample unit in the emitter's
  shape under `tests/abi/` builds through `cmake/module` into a shared library with exactly one exported symbol and
  no undefined references (`-z defs`), and a CTest loads it with `dlopen` and reads a valid descriptor.

### B: Host runtime, game glue, `dq8-game`
**Owns:** `src/runtime/**`, `src/game/**`, `src/host/**` (with READMEs and CMake), the top-level `CMakeLists.txt`
lines that add them, `tests/unit/test_{dispatch,guest_memory,game_module_desc,ee_runtime_contract,kernel_calls}.cpp`.
- Implement the host runtime as designed above. Until session A lands, the contract test may use a minimal local
  macro shim under `tests/unit/support/` for the few macros it needs; switch to A's header at merge time.
- `test_ee_runtime_contract.cpp`: a hand-written unit in the emitter's shape (resume `switch`, `dispatchGuestBranch`
  for `jal`, `jr $ra` as `ctx->pc = ra; return;`) with an in-process descriptor. Cases: call and return through
  dispatch; a jump that unwinds and re-enters through the `jal+8` resume slot; `pc == 0` ends the program; syscall
  stop with expected registers; named-call stop; missing-function message; a store to a scratchpad literal reaches
  the scratchpad. `kernel_calls`: number ↔ name table from the public SDK numbering, every name present in
  `PS2_SYSCALL_LIST`.
- **Done when:** unit tests green on GCC/Clang; `dq8-game --elf` runs the contract unit; `dq8-game --iso` verifies the
  disc, loads modules from `--module-dir`, and reports every descriptor problem against the region profile.

### C: The setup pipeline in C++
**Owns:** `src/core/elf/elf_writer.*`, `src/core/overlay/overlay_elf.*`, `src/core/platform/**`, `src/core/recomp/**`,
`include/dq8/core/{elf_writer,overlay_elf,process,recomp}.h` (and sub-headers), the `recomp` and `make-unit-elf`
commands in `src/cli/main.cpp` (split into `src/cli/recomp_commands.cpp` if it helps), `tests/unit/support/mips_asm.h`
(constexpr MIPS encoders, our own bytes), `tests/unit/test_{elf_writer,mips_asm,process,recomp_config,function_map}.cpp`,
READMEs of `src/core` and `src/cli`.
- `recomp_config`: `[general]` as above with `patch_syscalls = false`, `patch_cop0 = true`, `patch_cache = true`,
  `single_file_output = false`; `stubs = ["Name@0xADDR", ...]` for every map row whose name is in the compiled-in
  `ps2_call_list.h` (include it from `include/dq8/abi/ps2recomp/` once A lands; until then a local copy under
  `tests/unit/support/` is fine); `entry_points` from the map's link-branch list; `[jump_tables]` from the fragment.
- `process`: `posix_spawn` / `CreateProcessW`, argv vector (no shell), working directory, captured stdout/stderr,
  exit code, timeout and cancellation.
- `module_build`: `cmake -S <prefix>/cmake/module -B <work>/build/<unit> -G Ninja -D...` then `cmake --build -j N`,
  N = min(cores, (free RAM − 3 GiB) / 0.4 GiB); copy the library to `<work>/modules/<profile>/`.
- Map input format (from session D): `<unit>.csv` (`Name,Start,End,Size`), `<unit>.jump_tables.toml`
  (`[[jump_tables.table]]`), `<unit>.stats.toml` (`link_return_entries = [0x...]`, plus counts). Units are named
  `main`, `title`, `casino`, `viewer`, `battle`, `menu`, `shop`.
- **Done when:** unit tests green; on the real disc `dq8ctl recomp prepare` writes the seven unit ELFs/configs under
  `--work` and `generate` runs `ps2_recomp` on them (use `work/ps2recomp/build-gcc/ps2xRecomp/ps2_recomp` until E
  lands); `make-unit-elf` output is accepted by `ps2_recomp`.

### D: Function maps (Ghidra, local only)
**Owns:** `tools/ghidra/ExportFunctionMap.java`, `tools/ghidra/export_maps.sh`, a short section in `tools/README.md`.
- Exporter (postScript): arguments `<out.csv> <out.jump_tables.toml> <out.stats.toml> <unit_start> <unit_end>`.
  Functions only (no `LAB_`/`caseD_`/label rows), entry inside `[unit_start, unit_end)`, at least 4 bytes, End
  exclusive = min(body max + 1, next entry), `0x%08X`. `[jump_tables]`: for each computed `jr` (`rs != $ra`), the
  table address from the `lw` that loads the register, `base_register`, and the targets in order; unresolved sites,
  multi-range bodies and overlaps go to the stats file; `link_return_entries` = `bgezal`/`bltzal`/`bal` sites whose
  target is outside their function, plus 8.
- `export_maps.sh`: seven read-only headless runs: `main` on `work/ghidra/base` (`0x00100000 0x003D2680`), each overlay
  on `work/ghidra/s3-<overlay>` (`0x00461C80` to `0x00461C80 + 64 + text + data`; `dq8ctl info <BIN> --json` gives the
  sizes), output to `work/recomp/maps/`.
- **Done when:** seven maps, jump-table fragments and stats exist; `ps2_recomp` exits 0 on all seven (with C's
  `dq8ctl recomp generate`, or a hand-written TOML in `work/` before C lands); the counts are reported (functions,
  resolved tables, unresolved sites, link-return entries, unhandled instructions versus S1's 7,948).

### E: Recompiler build, end-to-end tests, CI
**Owns:** `cmake/Ps2Recomp.cmake`, `cmake/Dq8Options.cmake` (new options), `patches/ps2recomp/README.md`,
`tests/e2e/**`, `.github/workflows/ci.yml`, the install rules for `ps2_recomp`.
- Build `ps2_recomp` offline from the submodules (ExternalProject as above), expose `DQ8_PS2_RECOMP_EXECUTABLE`,
  install it next to `dq8-game`.
- `tests/e2e/`: a fixture generator (links core, uses C's `mips_asm.h` and `ElfWriter`) writing a hand-assembled ELF
  and CSV (`lui a0,0x1234; ori a0,a0,0x5678; jal helper; nop; addiu v1,zero,0x3C; syscall` and `helper: addiu
  v0,a0,1; jr ra`), a `bgezal` variant using `entry_points`, and a second unit at a synthetic window for an overlay
  round trip; then `dq8ctl recomp generate/build` and `dq8-game --elf ... --json` must stop at syscall `0x3C` with
  `a0 = 0x12345678`, `v0 = 0x12345679`; `nm -D --defined-only` shows one export per module.
- CI: a `recomp-e2e` job on ubuntu-24.04 with GCC 13 (`DQ8_BUILD_RECOMP=ON`, `DQ8_BUILD_E2E=ON`, `ctest -L e2e`).
- **Done when:** `ps2_recomp` builds from the submodules with no network; the e2e tests pass locally and in CI. Start
  with the recompiler build immediately; the e2e step needs A, B and C merged.

## Acceptance (coordinator, after the merges)
1. Seven maps from the exporter; `ps2_recomp` exit 0 on all seven; unhandled instructions far below 7,948.
2. Seven modules build with `-z defs` and one export each; CPU time recorded (S1 estimate: about 6,000 CPU-s with the PCH).
3. `dq8-game --iso "$DQ8_ISO" --module-dir work/recomp/modules/SLUS_21207 --stop-at-first-syscall` validates all seven
   descriptors, runs from `0x00100008`, stops at the first syscall with a context dump, exit 0.
4. CI green: unit tests (GCC, Clang, MSVC, ASan) and `recomp-e2e`.
5. `check_no_game_data.py --tree --deep`, `pre-commit run --all-files`, `reuse lint` clean.
6. Docs updated: architecture §4.2 (every `jal` dispatched), §4.3 (per-unit tables in the window), §13.3 and
   tech-stack §2/§6.2 (the exception deviation; no per-file chunking without an emitter patch), tech-stack §5 and
   notices (five recompiler dependencies with the analyzer off), brief §11, CLAUDE.md.

## Results

Measured on 2026-10-20 on the development machine (Ryzen 5 5600, 6 cores / 12 threads, GCC 16), NTSC-U disc
`SLUS_21207`, with `dq8ctl recomp all` and `dq8-game` built from `main` at `87b04e1`. Addresses and counts only.

**Function maps (session D, `tools/ghidra/ExportFunctionMap.java`).**

| Unit | Functions | Jump tables used by the recompiler | Unresolved computed jumps |
|---|---|---|---|
| main | 9,600 | 115 of 116 (plus 9 the recompiler resolved itself) | 18 in Ghidra, 1 left to runtime dispatch |
| title | 69 | 0 | 0 |
| casino | 238 | 10 of 10 | 0 |
| viewer | 16 | 0 | 0 |
| battle | 612 | 6 of 7 | 0 |
| menu | 232 | 3 of 4 | 1 |
| shop | 399 | 0 | 0 |

No link-branch return entries were needed in any unit. Exporting all seven maps takes about 32 s.

**Recompile (`ps2_recomp` built from the submodules, about 1 minute).** Exit 0 on all seven units, **0 unhandled
instructions** (S1's heuristic scan had 7,948), every branch and case target inside an exported range. Generated
output: 9,600 + 1,566 overlay functions; the main unit's dispatch table has 161,762 entries over
`0x00100008`–`0x003CF8EC` (S1's heuristic map produced 739,742 slots). Recompiling takes 1.5 s for main and well
under a second per overlay.

**Module build (`cmake/module`, 12 jobs, precompiled header).** main 412 s wall (57 MB), battle 25 s, casino 17 s,
menu 11 s, shop 11 s, title 5 s, viewer 2 s. Every module exports exactly one symbol (`dq8_game_module_describe`) and
has no undefined references outside the C/C++ runtime. S1 estimated about 15 minutes with a PCH; the measured build is
about 7 minutes.

**First run.** `dq8-game --iso <disc> --module-dir work/recomp/modules/SLUS_21207 --stop-at-first-syscall` verifies
the disc, loads and validates all seven modules against the region profile (overlay tables inside the window
`[0x00461C80, 0x00489A80)`, none resident), loads the seven ELF segments and runs from `0x00100008`. The start-up
code zeroes bss inline and stops at the **first syscall: `0x3C` `SetupThread`, at `0x001001C8`**, with `$a0` = the
global pointer `0x003D9770`, `$a1` = the stack argument `0x01F40000` (its bottom, as M3 found), `$a2` = stack size
`0x000C0000`, `$a3` = argument block `0x0041F680`, and `$v0` = `0x00489A80` (the end of bss and of the overlay
window, where S3 found the heap begins). No MMIO was touched. Exit 0. This is where M3 starts: `SetupThread` is the
first kernel service to implement.

**CI.** Unit tests on Linux GCC 13, Clang 18 and ASan, the lint/policy job and the new `Linux GCC 13 recompile
end-to-end` job pass. Windows MSVC failed on warning C4324 for the 16-byte-aligned `R5900Context`; suppressed in
`r5900_context.h` (scoped `#pragma warning`) in the results commit.

**Follow-ups found during M2** (not blocking): split overlay functions in the Ghidra projects need the per-call-site
overlay attribution S3 left open; two upstream helpers look wrong against the hardware (`FPU_CVT_W_S` rounds instead of
truncating, the variable MMI shifts act on 32-bit lanes); the JSON writer and region lookup are duplicated between
`dq8ctl` and `dq8-game`; the module template's MSVC path is unverified on a real Windows build of a game module.
