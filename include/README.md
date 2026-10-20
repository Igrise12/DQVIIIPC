# include/

Public headers. Stable, documented contracts shared by the runtime, features and generated game code (ABI headers live under dq8/). Keep this surface small. See Docs/architecture.md §13.

## dq8/abi/: the game-module ABI

The contract between the host and a game module, the shared library built on the player's machine from one recompiled code unit (architecture.md §2.1, §13.3; `Docs/milestones/M2-first-recompile.md`). Plain C at the boundary; nothing in a module refers to the host by symbol.

| Header | What it is |
|---|---|
| `game_module.h` | `dq8_game_module_desc` (what a module exports through `dq8_game_module_describe`), `dq8_runtime_api` (what the host gives generated code), ABI version 1. C. |
| `ps2recomp/` | An include root with the names PS2Recomp's output includes, so generated code compiles against our adapter instead of upstream's runtime. |
| `ps2recomp/ps2_runtime.h` | `PS2Runtime`: two pointers (`api`, `host`) and inline members that forward to `dq8_runtime_api`. |
| `ps2recomp/r5900_context.h` | `R5900Context`, `PS2Exception`, `getRegU32`, field for field as upstream (GPL-3.0-only). |
| `ps2recomp/ps2_address.h` | The EE memory map and `ps2_is_special_address`: everything that is not RAM or a RAM mirror goes to the host. |
| `ps2recomp/ps2_runtime_macros.h` | Every macro and helper the emitter writes (`READ*`/`WRITE*`, `FAST_*`, `GPR_*`, integer, MMI, VU and FPU helpers), adapted from upstream (GPL-3.0-only). `FAST_*` are routed through `PS2Runtime::isSpecialAddress`; the header lists the other changes. |
| `ps2recomp/ps2_call_list.h` | Upstream's `PS2_SYSCALL_LIST` and `PS2_STUB_LIST`, byte for byte (`abi.call_list_parity` checks; `.license` sidecar, kept out of clang-format). |
| `ps2recomp/ps2_syscalls.h`, `ps2_stubs.h` | `ps2_syscalls::<name>` and `ps2_stubs::<name>` for every listed name, forwarding to `api->call_named` with `DQ8_CALL_SYSCALL` or `DQ8_CALL_STUB`; `TODO` and `TODO_NAMED` in `ps2_stubs` forward with `DQ8_CALL_TODO`. |
| `ps2recomp/ps2_named_call.h` | How those names forward: each is an alias of `dq8_named_call<kind, "name">`, whose constructor makes the call (an alias because upstream's list repeats a name). |
| `ps2recomp/ps2_log.h` | `PS_LOG_ENTRY` as a no-op. |

Rules: a change to the descriptor, the api table, `R5900Context` or the meaning of a field bumps `DQ8_GAME_MODULE_ABI_VERSION` and gets an ADR. `ps2_call_list.h` is never edited by hand; copy it again when the PS2Recomp submodule moves. The headers must compile on their own under the project's warnings (`tests/abi` builds each one alone). Generated code needs `-msse4.1`; the GPR and memory macros do not, so host tests can use them without it.

Modules are built by `cmake/module/` (one build tree per unit; its `CMakeLists.txt` lists the parameters) and tested by `tests/abi/`.
