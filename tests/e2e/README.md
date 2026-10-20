# tests/e2e/

End-to-end tests of the recompile pipeline on hand-assembled MIPS code: `ps2_recomp` → one game module per code
unit (`cmake/module`) → `dq8-game --elf`. Label `e2e`; built with `-DDQ8_BUILD_RECOMP=ON -DDQ8_BUILD_E2E=ON`;
Linux only for now. CI runs them in the `recomp-e2e` job. **No game data:** `fixture_gen.cpp` assembles every
instruction with `tests/unit/support/mips_asm.h`, and every address, size and name is ours.

## Fixtures

`dq8_e2e_fixtures <out>` writes three fixtures. In each, the program sets `a0 = 0x12345678`, reaches a helper that
sets `v0 = a0 + 1` and returns, puts `0x3C` in `v1` and executes `syscall`.

| Fixture | Units | What it covers |
|---|---|---|
| `call` | `main` at `0x00100000` | `jal` through `dispatchGuestBranch`, return, resume at `jal + 8` |
| `bgezal` | `main` | a taken `bgezal`: the recompiler makes no resume entry for its return, so the map's `link_return_entries` must reach the config's `entry_points` (without it the `switch` has no case for the return) |
| `overlay` | `main`, `ovl` (overlay id 1) | a call from the main unit into an overlay unit at a synthetic window `0x00300000`..`0x00310000`; the overlay is written as an MWo3 container and turned into its unit ELF by `make_overlay_unit_elf` |

For each fixture: `units/<unit>.elf` and the Ghidra exporter's map format, `maps/<unit>.csv`,
`<unit>.jump_tables.toml` and `<unit>.stats.toml` (overlays also get `units/<unit>.mwo3`).

## Tests

| Test | Checks |
|---|---|
| `e2e.fixtures` | the generator runs |
| `e2e.<fixture>.generate` | `dq8ctl recomp generate` runs `ps2_recomp` on every unit |
| `e2e.<fixture>.build` | `dq8ctl recomp build` builds one module per unit |
| `e2e.<fixture>.exports` | `nm -D --defined-only` shows exactly one export per module, `dq8_game_module_describe` |
| `e2e.<fixture>.run` | `dq8-game --elf ... --stop-at-first-syscall --json` exits 0 and reports syscall `0x3C` with `a0 = 0x12345678`, `v0 = 0x12345679`, `v1 = 0x3C` |

`run_e2e.cmake` runs one step; CTest fixtures order the steps. Work files go to `<build>/tests/e2e/work/`.

**Status (2026-10-20):** the generator and `e2e.fixtures` run, and `ps2_recomp` accepts all four units
(exit 0, no unhandled instructions). The four pipeline steps are written against the M2 brief and register only
once `dq8-game` and `cmake/module` exist; the options they pass that the brief leaves open (how `dq8ctl` gets a
unit ELF without a disc, where `build` finds the template, the shape of `dq8-game --json`) are grouped at the top
of `run_e2e.cmake`.

```bash
cmake --preset dev-gcc -DDQ8_BUILD_RECOMP=ON -DDQ8_BUILD_E2E=ON && cmake --build --preset dev-gcc --parallel
ctest --test-dir build/dev-gcc -L e2e --output-on-failure
```
