# src/runtime/

The PS2 machine, implemented natively: ee/ (memory, context, dispatch, overlays), kernel/ (syscall HLE, threads), iop/ (IOP service HLE), dma/ (DMAC, VIF, GIF), gs/ (backend interface and backends), vu/ (VU1), spu2/ (audio), io/ (CDVD, memory card, pad), movie/ (video), platform/ (SDL3 window, input, audio).

Library `dq8_runtime` (`dq8::runtime`), namespace `dq8::rt`, links `dq8::core` only. Headers are included as `"runtime/<area>/<file>.h"`; the module ABI it implements is `include/dq8/abi/` (`game_module.h`, `ps2recomp/`).

What exists (milestone M2, the first recompile: run the main unit from its entry to its first syscall):

| Area | Files | Purpose |
|---|---|---|
| stop | `ee/guest_stop.*` | `StopInfo`: reason, message, pc, the address it is about, the kernel call's number, name and `$a0..$a3`, the last dispatched branch, and the thread that stopped (`thread_id`: 0 the scheduler's stack, -1 the handler fiber). `to_string(StopReason)` is the stable `--json` spelling. Since M3 a stop is a fiber switch; `GuestStop` remains the exception thrown where nothing can switch (trap-mmio, a stop off any fiber) and is caught on the fiber that threw it or by the scheduler loop. |
| memory | `ee/guest_memory.*` | 32 MiB RDRAM and the 16 KiB scratchpad. The host side of the generated code's slow path (`load8..load128`, `store8..store128`) for every address `ps2_is_special_address()` accepts: scratchpad, RAM mirrors, and the MMIO log for device ranges (loads return 0, stores are dropped, the first access to each address is recorded with region, width, value and pc, later ones counted; `set_trap_mmio` stops the guest instead). |
| ELF | `ee/elf_loader.*` | Places PT_LOAD segments through `core::parse_elf_headers`: `filesz` copied, `memsz - filesz` zeroed, zero-filesize segments (the overlay-window reservations) zeroed in full. EM_MIPS `ET_EXEC` only; RAM (one mirror, no wrap) or scratchpad. |
| context | `ee/context.*` | `reset_context()`: power-on `R5900Context`, then `pc = entry`, `sp = 0x01FFFFF0`, `a0 = a1 = ra = 0`. GPR accessors and the text dump. |
| dispatch | `ee/dispatch_table.*`, `ee/overlay_manager.*`, `ee/dispatcher.*` | A unit's dense table (one slot per 4 bytes, a view over the module's array). `OverlayManager`: the window (from the game layer), registered overlay units, `set_resident` / `clear_resident` with a generation counter, `describe_miss`. `Dispatcher`: main table, else window -> resident table, else a miss with a one-sentence reason. |
| EE runtime | `ee/ee_runtime.*` | `EeRuntime`: the `dq8_runtime_api` table (one trampoline per `PS2Runtime` member; `host` is the `EeRuntime`), `dispatch_guest_branch`, `checkpoint`, `handle_syscall` and `call_named` through `KernelServices` (SetSyscall guest handlers as nested calls), the exception and unsupported-service stops, and the fiber run loops (`run_thread`, `call_guest`). |
| modules | `ee/game_module.*`, `platform/dynlib.*` | `GameModule::open` (`dlopen(RTLD_NOW \| RTLD_LOCAL)` / `LoadLibraryW`, the one export, the descriptor), `validate_descriptor` against a `UnitSpec` (every problem in one pass; a bad magic, ABI version or size alone), `load_modules` (all units, all problems), module file naming (`dq8_unit_<name>` with the platform's prefix and suffix). |
| kernel | `kernel/kernel_calls.*` | The kernel call number <-> name table (public SDK numbering as PS2Recomp's numeric dispatcher has it; negative numbers are the interrupt-context calls; aliases marked). M2's `KernelHle` is gone: `KernelServices` replaced it in M3. |

M3 (Docs/milestones/M3-boots.md) adds the machine around it. Day 0 put these on `main`; sessions A–E implement the
rest of the contracts listed there:

| Area | Files | Purpose |
|---|---|---|
| machine | `machine/clock.*`, `machine/events.*`, `machine/cost_model.h`, `machine/spin_detector.h` | Virtual time in EE cycles (`VirtualClock`, exact NTSC/PAL `VideoTiming`), the deterministic `EventQueue`, the named cost model charged at safe points, and polling-loop detection (ADR-0007). |
| devices | `machine/interrupts.*`, `machine/mmio.*`, `machine/machine_services.h` | `InterruptController` (INTC and DMAC status with their write semantics), `MmioBus` (physical device ranges), and the `MachineServices` / `IDevice` contract every device is attached through. |
| kernel | `kernel/kernel_services.*`, `kernel/kernel_layout.h` | The kernel-call registry that replaces `KernelHle` (aliases, SetSyscall guest handlers first, unimplemented calls stop) and `KernelCallContext` (arguments and sign-extended results); the HLE kernel RAM layout. |
| fibers | `kernel/fiber.h`, `kernel/fiber_{minicoro,ucontext,win32}.cpp`, `kernel/fiber_stack.*` | `rt::Fiber` (ADR-0007): one backend per build (`DQ8_FIBER_BACKEND`), stacks of `FiberStackSpec::reserve` bytes (8 MiB, 32 MiB under sanitizers) with a guard page below, `headroom()` for the dispatch guard. Each backend annotates its switches for AddressSanitizer. |
| scheduler, kernel | `kernel/ee_kernel.h`, `kernel/{scheduler,kernel_objects}.cpp`, `kernel/kernel_abi.h` | `EeKernel` (adapted from PS2Recomp's `EeScheduler`): guest threads on fibers, strict-priority FIFO selection, the safe points, the handler fiber that runs INTC, DMAC and alarm handlers, threads, semaphores, event flags, alarms on hblank ticks, handler chains, the HLE kernel RAM sentinel, the deadlock report and the no-progress watchdog. |
| services | `kernel/ee_services.h`, `kernel/services_{thread,sync,interrupt,system}.cpp` | The kernel's own `KernelServices` registrations (owner `kernel`): threads, synchronisation, alarms, interrupt handlers, and the system calls (SetupThread/Heap, OSD configuration, SetSyscall and the syscall-table helpers, documented noops, the exit-path stops). |
| machine | `machine/machine.cpp` | `Machine`: the clock, events, interrupts, MMIO bus and kernel registry, the vblank events in `VblankPhase` order, spin skipping, and resumable `run()` slices. |
| declared | `gs/*.h` (backends D), `ee/overrides.h`, `platform/platform.h` | Interfaces the other M3 sessions implement: the EE devices, the IOP services, disc and pad input, the GS pipeline and backends, overrides, the SDL3 platform. |

M3 session B, the EE devices and the GS front end (`make_ee_devices`, in attach order):

| Area | Files | Purpose |
|---|---|---|
| bus | `ee/guest_memory.*`, `dma/register_block.h` | `GuestMemory::set_mmio_bus`: device addresses below kseg2 go to the `MmioBus` by physical address and cost `mmio_access`; unhandled ones keep M2's log and trap. `RegisterWindow` turns 8- to 128-bit guest accesses into a block's 32-bit register accesses. |
| timers, INTC | `machine/ee_timers.*`, `machine/intc_regs.*` | The four EE timers counted lazily from the virtual clock (bus, /16, /256, hblank), compare and overflow as events, INTC 9-12; I_STAT/I_MASK forwarded to `InterruptController`, INTC 2/3 in the intc vblank phase. |
| DMA | `dma/dmac.*`, `dma/vif.*`, `dma/gif.*` | DMAC channels 0-2 and 8-9 (normal, chains with every tag, SPR destination chains, interleave) with synchronous delivery and completion events; SIF0/SIF1 registers stored for the IOP link. VIF0/VIF1 as a streaming VIFcode interpreter. The GIF cuts packets at EOP per path, masks PATH3, and acts on SIGNAL/FINISH/LABEL/TRXDIR. |
| VU | `vu/vu_interpreter.h`, `vu/vu_{core,lower,upper}.cpp`, `vu/vu_host.h`, `vu/vu_units.*` | PS2Recomp's VU interpreter behind `IVuHost` (XGKICK, decode-cache generation, reserved instructions); VU0/VU1 memory at 0x11000000; MSCAL runs a program to its end. |
| GS | `gs/gs_priv_device.*`, `gs/gs_pipeline.cpp`, `gs/gs_backends.*`, `gs/gs_trace_format.h`, `gs/png_writer.cpp` | The privileged page with the gs and kernel vblank phases and the GS kernel calls; `GsPipeline` (hash chain, render thread, mailbox, PNG dumps, inline mode); the `null` and `trace` backends. |

Adapted upstream code (PS2Recomp eb8688b) is tagged `GPL-3.0-only`; register behaviour taken from PCSX2 says so where it is used. The device tests run on `tests/unit/support/ee_rig.h`.

M3 session C, the IOP as native services (ADR-0011):

| Area | Files | Purpose |
|---|---|---|
| IOP | `iop/iop.*`, `iop/sif_link.*`, `iop/sif_protocol.h`, `iop/iop_rpc.*`, `iop/iop_modules.*`, `iop/iop_memory.*`, `iop/services/*` | The IOP as native services (ADR-0011, session C): the SIF registers and the `sceSif*` kernel calls, SIF1 transfers into IOP RAM and SIF0 packets back (one per EE arming, at virtual times), the command layer (init handshakes, IOP reset), RPC bind/call/other-data, the module registry driven by the profile's `[iop]` rows, and the servers: fileio (asynchronous completion records), iopheap, loadfile, cdvdfsv, the pad server, mcserv (adapted from PS2Recomp, GPL-3.0-only) and null servers. Layouts: `Docs/milestones/M3-boots.md`, "Protocol notes". |
| disc | `io/disc_source.h`, `io/iso_disc_source.cpp` | `make_iso_disc_source`: path lookup (any separator, case, `;1`) and sector reads over a verified ISO image. |

The dispatch contract (PS2Recomp `PS2Runtime::dispatchGuestBranch` at eb8688b, and the milestone doc):

- `dispatch_guest_branch` sets `ctx->pc = target`, counts a dispatch and charges its cost. A jump (`J`, `JR`) sets the fiber's `unwinding` flag and returns `false`: the generated callers return and the run loop continues at `ctx->pc`. A call (`JAL`, `JALR`) is a safe point, then runs the target on the fiber's stack. Afterwards it returns `false` while `unwinding` is set; it returns `true` when `ctx->pc == fallthrough` (a plain return); anything else (pc 0, a changed `$ra`, a `jr` to the callee's own entry) sets `unwinding` and returns `false`. When it is `false` the caller returns too, and the run loop re-enters through the resume entry at `jal+8` (a table slot pointing at the owning function, whose `switch (ctx->pc)` jumps to the label). The flag replaces M2's "pc unchanged means returned" rule, which mistook a `jr` to the callee's own entry for a return.
- Safe points switch fibers inside the call and return normally: `ee_checkpoint_due` charges `cycles × backedge_scale` and always returns 0; dispatched calls, kernel calls (`block_current`, preemption after the call) and each fiber's run loop are the others. An interrupt is taken only when IE and EIE are set and EXL/ERL are clear, on the handler fiber (`$ra` 0, `$sp` `hle_handler_stack_top`).
- A guest thread's run loop: `pc == 0` ends the program for the boot thread (exits any other thread), otherwise look the pc up, count, call. A C++ exception from generated code is caught on its fiber and becomes a `guest-error` stop after the handler is left.
- `handle_syscall` takes the number from the instruction's code field, or `$v1` when that is 0; `KernelServices` resolves it (a SetSyscall guest handler first, then the native service) and anything unresolved stops with number, name and arguments. `stop_at_first_syscall` stops at every syscall and by-name kernel call. By-name calls (`ps2_syscalls::`, `ps2_stubs::`, `TODO`) arrive through `call_named` and resolve the same way.

Differences from upstream, all deliberate: a miss stops at once with the source branch in the message (upstream reports and returns, and its scheduler stops later); there is no missing-function policy and no interpreter fallback; a dispatch budget (`max_dispatches`) and a host-stack guard (`max_call_depth`, default 4096 nested guest calls, plus a 256 KiB headroom check on the fiber's stack) turn runaway guests into stops instead of hangs or crashes; trap, break and every exception the generated code signals stop the guest (no COP0 exception delivery yet); TLB instructions, VU0 microprograms and GIF DMA kicks stop as `unsupported`; upstream's throw-to-unwind transfers, host threads, wall-clock pacing and time slices are replaced by fibers, the virtual clock and strict-priority scheduling; a SetSyscall handler that jumps to an address with no function stops instead of completing silently.

Rules:

- Nothing here knows Dragon Quest: no addresses, overlay names, unit names or profile ids. The game layer (`src/game/`) turns a region profile into `UnitSpec`s and an `OverlayWindowSpec`.
- Nothing crosses the module boundary except the C ABI in `include/dq8/abi/`. Since M3 the host's stops are fiber switches; only the emitter's inline `throw` and trap-mmio's `GuestStop` still unwind through module frames, so host and modules still share the C++ runtime (milestone doc, "M2 deviation").
- Host frames on a guest fiber are abandonable (`kernel/fiber.h`): nothing that may stop or block keeps owned heap memory, a lock or a handle in a local or a temporary of the statement that stops.
- Guest memory never holds a host pointer. Guest-visible behaviour goes through the dedicated helpers, never raw host float operations (architecture §4.4).
- Every component is unit-tested without game data (`tests/unit/test_{dispatch,guest_memory,game_module_desc,ee_runtime_contract,kernel_calls,kernel_services,machine_primitives,m3_contracts,fiber,scheduler,kernel_threads,kernel_sync,kernel_interrupts,kernel_system}.cpp`); device tests use `tests/unit/support/fake_machine.h`, execution-core tests `tests/unit/support/machine_harness.h`.
