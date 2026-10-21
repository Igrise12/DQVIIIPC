# src/runtime/

The PS2 machine, implemented natively: ee/ (memory, context, dispatch, overlays), kernel/ (syscall HLE, threads), iop/ (IOP service HLE), dma/ (DMAC, VIF, GIF), gs/ (backend interface and backends), vu/ (VU1), spu2/ (audio), io/ (CDVD, memory card, pad), movie/ (video), platform/ (SDL3 window, input, audio).

Library `dq8_runtime` (`dq8::runtime`), namespace `dq8::rt`, links `dq8::core` only. Headers are included as `"runtime/<area>/<file>.h"`; the module ABI it implements is `include/dq8/abi/` (`game_module.h`, `ps2recomp/`).

What exists (milestone M2, the first recompile: run the main unit from its entry to its first syscall):

| Area | Files | Purpose |
|---|---|---|
| stop | `ee/guest_stop.*` | `GuestStop` (a C++ exception, M2 only) carrying `StopInfo`: reason, message, pc, the address it is about, the kernel call's number, name and `$a0..$a3`, and the last dispatched branch. `to_string(StopReason)` is the stable `--json` spelling. |
| memory | `ee/guest_memory.*` | 32 MiB RDRAM and the 16 KiB scratchpad. The host side of the generated code's slow path (`load8..load128`, `store8..store128`) for every address `ps2_is_special_address()` accepts: scratchpad, RAM mirrors, and the MMIO log for device ranges (loads return 0, stores are dropped, the first access to each address is recorded with region, width, value and pc, later ones counted; `set_trap_mmio` stops the guest instead). |
| ELF | `ee/elf_loader.*` | Places PT_LOAD segments through `core::parse_elf_headers`: `filesz` copied, `memsz - filesz` zeroed, zero-filesize segments (the overlay-window reservations) zeroed in full. EM_MIPS `ET_EXEC` only; RAM (one mirror, no wrap) or scratchpad. |
| context | `ee/context.*` | `reset_context()`: power-on `R5900Context`, then `pc = entry`, `sp = 0x01FFFFF0`, `a0 = a1 = ra = 0`. GPR accessors and the text dump. |
| dispatch | `ee/dispatch_table.*`, `ee/overlay_manager.*`, `ee/dispatcher.*` | A unit's dense table (one slot per 4 bytes, a view over the module's array). `OverlayManager`: the window (from the game layer), registered overlay units, `set_resident` / `clear_resident` with a generation counter, `describe_miss`. `Dispatcher`: main table, else window -> resident table, else a miss with a one-sentence reason. |
| EE runtime | `ee/ee_runtime.*` | `EeRuntime`: the `dq8_runtime_api` table (one trampoline per `PS2Runtime` member; `host` is the `EeRuntime`), `dispatch_guest_branch`, `handle_syscall`, `call_named`, the exception and unsupported-service stops, and `run()`. |
| modules | `ee/game_module.*`, `platform/dynlib.*` | `GameModule::open` (`dlopen(RTLD_NOW \| RTLD_LOCAL)` / `LoadLibraryW`, the one export, the descriptor), `validate_descriptor` against a `UnitSpec` (every problem in one pass; a bad magic, ABI version or size alone), `load_modules` (all units, all problems), module file naming (`dq8_unit_<name>` with the platform's prefix and suffix). |
| kernel | `kernel/kernel_calls.*`, `kernel/kernel_hle.*` | The kernel call number <-> name table (public SDK numbering as PS2Recomp's numeric dispatcher has it; negative numbers are the interrupt-context calls; aliases marked) and `KernelHle`, a by-name handler registry that is empty in M2. |

M3 (Docs/milestones/M3-boots.md) adds the machine around it. Day 0 put these on `main`; sessions A–E implement the
rest of the contracts listed there:

| Area | Files | Purpose |
|---|---|---|
| machine | `machine/clock.*`, `machine/events.*`, `machine/cost_model.h`, `machine/spin_detector.h` | Virtual time in EE cycles (`VirtualClock`, exact NTSC/PAL `VideoTiming`), the deterministic `EventQueue`, the named cost model charged at safe points, and polling-loop detection (ADR-0007). |
| devices | `machine/interrupts.*`, `machine/mmio.*`, `machine/machine_services.h` | `InterruptController` (INTC and DMAC status with their write semantics), `MmioBus` (physical device ranges), and the `MachineServices` / `IDevice` contract every device is attached through. |
| kernel | `kernel/kernel_services.*`, `kernel/kernel_layout.h` | The kernel-call registry that replaces `KernelHle` (aliases, SetSyscall guest handlers first, unimplemented calls stop) and `KernelCallContext` (arguments and sign-extended results); the HLE kernel RAM layout. |
| declared | `kernel/{fiber,scheduler}.h`, `machine/{machine,ee_devices}.h`, `iop/*.h`, `io/*.h`, `gs/*.h`, `platform/platform.h` | Interfaces the M3 sessions implement: fibers and the scheduler, the `Machine`, the EE devices, the IOP services, the disc, the GS pipeline and backends, the SDL3 platform. |
| overrides | `ee/overrides.*`, `ee/dispatcher.*`, `ee/dispatch_table.*`, `ee/overlay_manager.*` | The dispatcher keeps an owned copy of the main table, and the overlay manager one of each overlay table. `Dispatcher::install_override` points one slot at a single trampoline that finds the override by `ctx->pc` (replace, hook_before, hook_after), so `resolve()` is unchanged. It accepts function entries only, not resume points, and checks an optional digest: main units at install time, overlay units on the first call after each residency change. One dispatcher per process holds overrides. The debug stale-return check (`OverlayManager::generation()` around calls from the window) is session A's, in `dispatch_guest_branch`. |
| input | `io/input_script.*`, `io/input_live.*`, `io/input_record.*` | `IInputSource`s: `.dq8in` scripts (`Docs/formats/dq8-input-script.md`; triggers fed by the host), the live per-vblank latch over the platform's pads, an idle source, and the `--record-input` recorder. |

The dispatch contract (PS2Recomp `PS2Runtime::dispatchGuestBranch` at eb8688b, and the milestone doc):

- `dispatch_guest_branch` sets `ctx->pc = target` and counts a dispatch. A jump (`J`, `JR`) returns `false`: the generated caller returns and the run loop continues at `ctx->pc`. A call (`JAL`, `JALR`) runs the target on the host stack; `pc == 0` afterwards returns `false` (program end), an unchanged `pc` counts as a return to `fallthrough`, and the result is `ctx->pc == fallthrough`. When it is `false` the caller returns too, and the run loop re-enters through the resume entry at `jal+8` (a table slot pointing at the owning function, whose `switch (ctx->pc)` jumps to the label).
- `run()` mirrors `EeScheduler::run` for one thread: `pc == 0` ends the program, otherwise look the pc up, count, call. It returns a `RunResult` for every stop, including a C++ exception from generated code (`guest-error`).
- `handle_syscall` takes the number from the instruction's code field, or `$v1` when that is 0, and stops with number, name and arguments unless the kernel HLE implements it (and `stop_at_first_syscall` is off). By-name calls (`ps2_syscalls::`, `ps2_stubs::`, `TODO`) arrive through `call_named` and stop the same way.

Differences from upstream, all deliberate: a miss stops at once with the source branch in the message (upstream reports and returns, and its scheduler stops later); there is no missing-function policy, no interpreter fallback, no checkpoint (`ee_checkpoint_due` is always false) and no scheduler or virtual clock; a dispatch budget (`max_dispatches`) and a host-stack guard (`max_call_depth`, default 4096 nested guest calls) turn runaway guests into stops instead of hangs or crashes; trap, break and every exception the generated code signals stop the guest (no COP0 exception delivery yet); TLB instructions, VU0 microprograms and GIF DMA kicks stop as `unsupported`.

Rules:

- Nothing here knows Dragon Quest: no addresses, overlay names, unit names or profile ids. The game layer (`src/game/`) turns a region profile into `UnitSpec`s and an `OverlayWindowSpec`.
- Nothing crosses the module boundary except the C ABI in `include/dq8/abi/`. In M2, C++ exceptions do cross it (`GuestStop`, and the emitter's inline `throw`): host and modules must share the C++ runtime (milestone doc, "M2 deviation"). M3 replaces `GuestStop` with fiber switches.
- Guest memory never holds a host pointer. Guest-visible behaviour goes through the dedicated helpers, never raw host float operations (architecture §4.4).
- Every component is unit-tested without game data (`tests/unit/test_{dispatch,guest_memory,game_module_desc,ee_runtime_contract,kernel_calls,kernel_services,machine_primitives,m3_contracts}.cpp`); device tests use `tests/unit/support/fake_machine.h`.
