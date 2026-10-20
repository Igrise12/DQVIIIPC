# tests/abi/

Tests of the game-module ABI (`include/dq8/abi/`) and the per-unit module template (`cmake/module/`). No game data:
the sample unit is synthetic code we wrote. Every test is labelled `unit` (so CI runs them) and named `abi.*`.

```bash
ctest --preset unit-dev-clang -R '^abi\.'
```

| What | Where | Checks |
|---|---|---|
| Standalone headers | `dq8_abi_standalone_headers` (an object library built with `all`) | each ABI header compiles alone, with the project's warnings and without `-msse4.1` |
| `abi.call_list_parity` | `check_call_list_parity.cmake` | `ps2_call_list.h` is byte-for-byte the PS2Recomp submodule's |
| `abi.<test case>` (macros, forwarders) | `test_macros.cpp` | RAM and mirrors stay in place; special addresses, `FAST_*` included, reach the host; GPR accessors; the overflow, MMI and FPU helpers; every listed name forwards to `call_named` |
| `abi.<test case>` (module) | `test_module_load.cpp` | `dlopen` the sample module, check its descriptor and table, run it to its syscall through `support/fake_host.h`, with calls that return and calls that unwind and re-enter through resume entries; an inline `throw` reaches the host |
| `abi.sample_module_exports` | `check_module_exports.cmake` | `nm -D --defined-only` lists `dq8_game_module_describe` and nothing else |

The last two rows need `DQ8_BUILD_ABI_MODULE_TEST` (on by default except on Windows). With it, the build runs
`cmake/module` on `sample_unit/` as an ExternalProject, exactly as `dq8ctl recomp build` will on a real unit.

`sample_unit/` is hand-written in the shape the recompiler (PS2Recomp `eb8688b2`) writes: one file per function with
its include order, the resume `switch`, `dispatchGuestBranch` for every `jal`, `jr $ra` as `ctx->pc = ra; return;`,
the stub bodies, and `register_functions.cpp` with resume entries. Its guest code is made up. It keeps the
recompiler's layout, so clang-format skips it (`.clang-format-ignore`). One deliberate difference: the skipped
function reaches `TODO_NAMED` through a namespace alias, because the no-game-data checker refuses the spelled-out
call as a marker of generated code.

`support/fake_host.h` is a minimal host: a `dq8_runtime_api` that records loads, stores and named calls, a
dispatcher that follows the dispatch contract in `Docs/milestones/M2-first-recompile.md`, and upstream's run loop.
It is not the runtime; `src/runtime` has the real one.
