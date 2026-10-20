# cmake/module/

The per-unit game-module build: a standalone CMake project that turns one unit's recompiled C++ (the main program or
one overlay) into a shared library exporting only `dq8_game_module_describe` (`include/dq8/abi/game_module.h`).
Installed with the host; `dq8ctl recomp build` runs it once per unit. `tests/abi` builds a synthetic unit with it.

```bash
cmake -S cmake/module -B work/recomp/build/main -G Ninja \
      -DDQ8_UNIT_NAME=main -DDQ8_UNIT_KIND=main -DDQ8_UNIT_OVERLAY_ID=0 \
      -DDQ8_UNIT_GENERATED_DIR=work/recomp/generated/main -DDQ8_UNIT_PROFILE_ID=SLUS_21207 \
      -DDQ8_UNIT_SOURCE_SHA1=<40 hex> "-DDQ8_GENERATOR=ps2recomp eb8688b2" -DDQ8_UNIT_ENTRY_POINT=0x00100008
cmake --build work/recomp/build/main -j N
cmake --install work/recomp/build/main --prefix work/recomp/modules/SLUS_21207   # optional copy
```

| Parameter | Meaning |
|---|---|
| `DQ8_UNIT_NAME` | `[a-z0-9_]+`; names the library (`dq8_unit_<name>`) and the renamed table globals |
| `DQ8_UNIT_KIND` | `main` or `overlay` |
| `DQ8_UNIT_OVERLAY_ID` | the MWo3 header's `overlay_id`; `0` for the main unit |
| `DQ8_UNIT_GENERATED_DIR` | the recompiler's output for this unit (`register_functions.cpp`, `ps2_recompiled_functions.h`, `ps2_recompiled_stubs.h`, one `.cpp` per function) |
| `DQ8_UNIT_PROFILE_ID`, `DQ8_UNIT_SOURCE_SHA1`, `DQ8_GENERATOR` | copied into the descriptor |
| `DQ8_UNIT_ENTRY_POINT` | the main unit's guest entry; `0` for overlays |
| `DQ8_ABI_INCLUDE_DIR` | where `dq8/abi/` lives; defaults to `../../include` from this directory (the source tree, or an install that keeps the same layout) |
| `DQ8_MODULE_PCH` (ON), `DQ8_MODULE_DEBUG_INFO` (OFF) | precompiled ABI and unit headers; debug information |

Every parameter is checked at configure time and all problems are reported together. Output:
`<build>/module/dq8_unit_<name>.so` (`.dll` on Windows), no `lib` prefix.

How it builds: C++20, hidden visibility; generated sources at `-O2 -msse4.1 -fexceptions -fno-rtti -w`,
`register_functions.cpp` at `-O0` without the PCH; the four `g_ps2RecompiledFunctionTable*` globals renamed to
`dq8_unit_<name>_table`, `_table_base`, `_table_end` and `_table_slot_count`; `dq8_unit_entry.cpp.in` fills the
descriptor. On ELF platforms the link uses `-z defs` (no undefined references to the host), `--exclude-libs,ALL` and a
version script that leaves exactly one exported symbol. The module shares the host's C++ runtime, because exceptions
cross the boundary in M2 (the milestone doc's documented deviation): never `-static-libstdc++`; on MSVC
`/EHs /GR- /MD` (that path is written but not yet built anywhere).
