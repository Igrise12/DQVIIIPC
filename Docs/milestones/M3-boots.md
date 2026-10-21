# M3: Boots (to the title screen)

| | |
|---|---|
| **Status** | Day 0 (contracts) done 2026-10-21; sessions A–E next, then bring-up |
| **Exit (brief §11)** | Memory, kernel HLE, threads, CDVD and overlay switching work; the game reaches the title screen; first pixels through the GS backend |
| **Owner choices (2026-10-21)** | Native IOP servers, with null servers for the two sound servers (ADR-0011) · SDL3 window plus headless mode · five parallel sessions, then serial bring-up · movies skipped (playback is M4) · ADR-0005 and ADR-0007 accepted |
| **Split** | **M3a**: headless boot to the title screen with the trace backend. **M3b**: paraLLEl-GS pixels, the window, live input |

Sessions read this file, not the plan. Contract changes go through the coordinator. Every rule in CLAUDE.md applies.
The most important is that **no game-derived content** goes into commits: addresses, sizes, counts, SDK names, server
ids and our own authored data are fine; bytes, disassembly, decompiled code, extracted files and frames are not.

## What exploration established

**The recompiled code (local generated tree, counts only).**
- The emitter calls `runtime->eeCheckpointDue()` **with no argument**: our adapter's default makes the host receive 32.
  This happens on backward internal branches only; main has 18,582 sites, battle 932, casino 684, menu 286, shop 187,
  title 65, viewer 0.
- Every `jal`/`jalr`/`jr` goes through `dispatchGuestBranch` (59,481 sites in main).
- A syscall is `ctx->pc = addr+4; runtime->handleSyscall(...)` and then execution continues inline. All 25 inline sites
  pass the number in `$v1`.
- A by-name kernel call (`ps2_syscalls::Name`, `ps2_stubs::Name`) sets `ctx->pc = $ra`, calls the host and returns.
- DI/EI are inline edits of bit 16 of `cop0_status` (16 sites). `cop0_count` is never read.
- There are 0 inline `throw`s, 0 `TODO` stubs and 0 `kickGifDmaChainFromMMIO` sites. `executeVU0Microprogram` has 2
  sites, `handleBreak` 339, `SignalException(overflow)` 27, `handleTLBWI` 2.
- **No ABI change is needed for M3**: `DQ8_GAME_MODULE_ABI_VERSION` stays 1 and M2's modules stay valid.

**Only kernel calls reach the host.**
- Every SDK library is recompiled game code: libsif RPC, libcdvd, libpad, libmc, libgraph, sceSdRemote, sceMpeg, libc.
- Main binds 53 kernel-call sites by name (8 of them `ps2_stubs::sceSif*`); the overlays bind none and contain no
  syscalls.
- Four bound spellings differ from the number table, so they need aliases or new rows: `SetGsCrt`, `isceSifSetDma`,
  `isceSifSetDChain`, `sceSifStopDma`.
- **Consequence:** the IOP side must speak the **SIF command and RPC protocol**.
  - EE→IOP packets arrive through `sceSifSetDma`/`DChain` into IOP RAM.
  - IOP→EE replies are data writes, then an end packet in the EE's command buffer, then a DMAC channel 5 (SIF0)
    interrupt. The game's own handler (installed in `sceSifInitCmd` at `0x00119680`) processes it.
  - The SIF register handshake is polled through `sceSifGetReg`/`SetReg`.

**Boot path (addresses from the spikes and save states).**

1. **crt0 `0x00100008`.**
   - Zeroes bss `0x003D2680`–`0x00489A80`.
   - `SetupThread` (gp `0x003D9770`, stack `0x01F40000`, size `0xC0000`, args `0x0041F680`). The stack argument is
     the stack **bottom**: the main stack is `0x01F40000`–`0x02000000`, and the heap ends at `0x01F40000`.
     M2's record called it the stack top; this corrects it.
   - `SetupHeap(0x00489A80, -1)`.
   - SDK init `0x00120B98`:
     - kernel patching (FindAddress, SetSyscall ×5 sites, GetEntryAddress, Copy, Get/SetOsdConfigParam, FlushCache);
     - a timer-2 INTC handler (line 11);
     - thread library init.
   - EI, then main `0x00160B00` (priority 40).
2. **main.**
   - EndOfHeap; 156 static constructors via `0x00100450`.
   - sceSifInitRpc and sceCdInit, then an IOP reset with `cdrom0:\MODULES\IOPRP300.IMG;1`, sync loops, and re-init.
   - **15 module loads** through the loadfile RPC from `cdrom0:\MODULES\`, in this order: SIO2MAN, PADMAN, MCMAN,
     MCSERV, LIBSD, SDRDRV, MODHSYN, MODMIDI, MODMSIN, CDVDSTM, SKSOUND (`maxtrack=16`), SKHSYNTH (`maxentry=16`),
     SKMIDI, SKMSIN, PCMPLAY.
3. **Servers bound** (every client busy-waits until its bind returns a non-zero server):

   | Server id | Service |
   |---|---|
   | `0x80000001` | fileio |
   | `0x80000003` | IOP heap |
   | `0x80000006` | loadfile |
   | `0x80000592`/`593`/`595`/`597` | cdvd init / S-command / N-command / SearchFile |
   | `0x8000059A` | disc ready |
   | `0x8000059C` | unknown |
   | `0x80000100`/`101` | padman, old libpad |
   | `0x80000400` | mcserv, bound lazily at the memory-card check |
   | `0x80000701` | sdrdrv/sceSdRemote; about 20 callers in the sound layer |
   | `0x123456` | PCMPLAY |

4. **GS init.**
   - sceGsResetGraph: SetGsCrt `0x02`, interlaced NTSC FRAME, then FIELD later.
   - GsPutIMR/GsGetIMR every frame, SetVSyncFlag `0x73`, and syscall `0x80` (probably `_GetGsDxDyOffset`).
   - A loop at `0x00109220` polls CSR FIELD.
   - Interrupt handlers: vblank-start (INTC 2), vblank-end (INTC 3, the PCMPLAY client), timers (INTC 11, 12).
   - INTC mask in every captured state: lines 1, 2, 3, 11, 12. DMAC handlers: channel 5 always; GIF (2) during movies.
5. **Archive init `0x00164520`.** `.HD6` indexes are read through fileio. `.DAT` files are found with sceCdSearchFile and
   read with sceCdRead at LSN + sector, polling sceCdSync and sceCdGetError.
6. **Mode machine `0x001693B0` and game-loop init `0x001690E0`.**
   - sceCdSearchFile for the 6 overlays (LSN cache `0x003F8850`).
   - Sound init: iopheap, sdrdrv bind, callback thread, sound thread at priority 30.
   - PCMPLAY bind plus a thread plus a vblank-end handler.
   - A worker at priority 38; graphics init `0x00160330`; memalign(16, `0x1A00000`).
   - Pad init `0x00166250` (pad DMA buffer near `0x003F9880`).
7. **TITLE load** through `0x00100330`:
   1. sceCdSeek, then fileio open/read/close of `BIN\TITLE.BIN` into the window;
   2. a call to the empty hook `0x00100320(dest)`;
   3. FlushCache(0)/(2);
   4. the constructor walk `0x00100DE0`.
8. **Mode 4 with TITLE (overlay id 6) resident** covers every screen before the title:
   - memory-card check `0x004631B0` (S7 passed it without input with an unformatted card);
   - first-run settings `0x00462C10` (Down, Cross, Cross);
   - movie scenes `0x00461E40`/`0x00462110`/`0x00463C50` calling the player API `0x001A2450`/`0x001A1A00`. The logos
     and the "flyover" are MVI movies, and Cross skips them;
   - then the 2D title.
9. **Title graphics.**
   - 512×448 PSMCT32 target copied into a PSMCT24 display buffer, with a PMODE 50/50 two-circuit merge, field
     interlaced, at 30 Hz.
   - Sprites, PSMT8 + CLUT, PSMT4HH glyphs aliased in Z pages, a 256×224 backdrop re-uploaded every frame.
   - DMA: GIF normal mode, VIF0/VIF1 chains (from the settings screen on), toSPR chains (from the logos on), SIF0/SIF1.
   - VU1 microcode is loaded, but little or no VU1 work happens before the title (*inferred*).

**Open unknowns M3 resolves in bring-up:**
- how the mode becomes 4;
- what the crt0 kernel-patch helpers need;
- the fileio reply structures (the unaligned-read fix-up);
- the padman v1 DMA frame layout;
- mcserv replies per card state;
- which sdrdrv and PCMPLAY replies the game checks;
- the identity of `0x8000059C`;
- the movie-skip return values;
- which handler paces the 30 Hz frame;
- the vblank length.

## Contracts (day 0, on `main`)

Implemented and tested on day 0, so sessions use them as they are:

| File | What it gives you |
|---|---|
| `src/runtime/machine/clock.{h,cpp}` | `Cycles`, `ee_clock_hz`, `bus_clock_hz`, `cycles_from_us`, `cycles_for_bytes`; `VirtualClock` (`charge`, `advance_to`, deadline, `due`, `force_safe_point`); `VideoTiming` (exact NTSC/PAL field and line starts, `vblank_end`, `fields_started`) |
| `src/runtime/machine/events.{h,cpp}` | `EventQueue`: `schedule(at, tag, callback)`, `cancel`, `run_due(now)` in (time, scheduling order), `next_time`, `pending()` for reports |
| `src/runtime/machine/interrupts.{h,cpp}` | `InterruptController`: INTC `I_STAT`/`I_MASK` (write 1 clears / toggles), DMAC `D_STAT` (CIS/CIM, SIS/MEIS/BEIS), `raise_intc`, `raise_dmac`, `pending()`, a raise hook; `IntcLine`, `DmaChannel` |
| `src/runtime/machine/mmio.{h,cpp}` | `IMmioDevice` (read/write by offset; `false` = unhandled), `MmioContext {pc, ctx}`, `MmioBus` (16-byte aligned ranges over physical `[0x10000000, 0x20000000)`, 4 KiB page table) |
| `src/runtime/machine/cost_model.h` | `CostModel "m3-v1"`: back-edge scale 1 (×32), dispatch 12, kernel call 150, MMIO access 8, DMA 4 per qword, RPC 20 µs + 40 MB/s, CDVD 100 µs + 10 MB/s, spin threshold 64, quantum ≈ 1 field |
| `src/runtime/machine/spin_detector.h` | `SpinDetector`: same pc, address and value N times in a row |
| `src/runtime/machine/machine_services.h` | **`MachineServices`** (memory, MMIO bus, clock, events, interrupts, kernel, cost, video, scheduler, vblank listeners by `VblankPhase` gs→kernel→iop→intc, `note_poll`, `note_activity`, `emit(MachineEvent)`, `[[noreturn]] stop`), **`IDevice`** (`attach`, `reset`), `IVblankListener`, `MachineEvent` |
| `src/runtime/kernel/kernel_services.{h,cpp}` | **`KernelServices`** registry (add by canonical name, aliases with an interrupt flag, SetSyscall guest handlers first, `resolve_number`, `resolve_name`, strict mode, call counts) and **`KernelCallContext`** (`arg(0..7)` = `$a0..$a3,$t0..$t3`, `ret()` sign-extends and keeps the upper 64 bits, `unimplemented()` stops) |
| `src/runtime/kernel/kernel_calls.cpp` | New rows: `0x06 LoadExecPS2`, `0x0D SetVTLBRefillHandler`, `0x6B sceSifStopDma`, `0x7B ExecOSD`, `0x80 GetGsDxDyOffset` (to be confirmed) |
| `src/runtime/kernel/kernel_layout.h` | HLE kernel RAM `[0x000E0000, 0x00100000)`, handler stack `[0x000F0000, 0x000FFFF0]`, sentinel word |
| `tests/unit/support/fake_machine.h` | `dq8::test::FakeMachine`: a full `MachineServices` without threads for device tests (`advance`, `run_events_until`, `fire_vblank_start/end`; `stop` throws `GuestStop`) |
| `include/dq8/core/region_profile.h` | Optional `[iop]` (`reset_image`, `[[iop.modules]] name/services`) and `[movie]` (`player_entries`, `scene_unit`, `scene_functions`), validated; SLUS-21207's rows filled in |

Declared on day 0 and implemented by the session in brackets:

| File | Contract |
|---|---|
| `src/runtime/kernel/fiber.h` [A] | `rt::Fiber`: `create(entry, arg, FiberStackSpec)`, `resume()` (scheduler only), `yield()`, `current()`, `finished()`, `stack_low()`. The two invariants are in the header |
| `src/runtime/kernel/scheduler.h` [A] | `Scheduler`: `current_thread_id`, `in_handler`, `request_reschedule`, `stop` (A may extend it) |
| `src/runtime/machine/machine.h` [A] | `Machine final : MachineServices`: `Machine(GuestMemory&, Dispatcher&, MachineConfig)`, `attach(IDevice&)`, `set_event_sink`, `boot(entry)`, `run(RunLimits) → SliceResult` (resumable), `ee()` |
| `src/runtime/machine/ee_devices.h` [B] | `make_ee_devices(EeDevicesConfig{gs::IGsSink*})`: the timers, INTC/DMAC registers, DMAC, VIF0/1, GIF, VU memory and VU1, GS privileged registers plus the GS kernel calls, and the SIO TTY, in attach order |
| `src/runtime/gs/{priv_regs,gs_backend,gs_sink,gs_pipeline}.h` [B; D for the parallel backend] | `PrivRegs` (8 KiB page, `PrivReg` offsets, CSR bits); `IGsBackend` (render thread only); `IGsSink` (machine thread: `gif_transfer`, `vsync`, `read_transfer`); `GsPipeline` (queue, render thread, mailbox, `stream_hash()`, `latest_frame`, `request_frame_dump`) |
| `src/runtime/iop/{iop,iop_memory,iop_server}.h` [C] | `Iop final : IDevice` built from `IopConfig {disc, input, modules, reset_image}`; `IopModuleSpec`; `IopMemory` (2 MiB, deterministic heap); `IopRpcServer`, `IopCallContext`, `RpcReply`, `EeWrite` |
| `src/runtime/io/{disc_source,pad_state}.h` [C; E for the input sources] | `IDiscSource`, `make_iso_disc_source(const core::iso::Image&)`; `PadState`, `PadButton`, `IInputSource::sample(port, vblank)` |
| `src/runtime/ee/overrides.h` [E] | `OverrideSpec {unit, address, kind, name, fn, code_bytes, code_sha1}`, `OverrideKind`, `OverrideCall` |
| `src/runtime/platform/platform.h` [D] | `IPlatform` (`pump_events`, `pad(port)`, `present(frame, aspect)`), `WindowConfig`, `create_sdl_platform` |
| `include/dq8/gs/gs_parallel_abi.h` [D] | The C ABI of `dq8-gs-parallel` (`dq8_gsp_*`, version 1) |

Day 0 also brought:
- submodules: `third_party/minicoro`, `third_party/SDL` (release-3.4.18), `third_party/stb`, `third_party/parallel-gs`
  (`cc6184a`, with Granite, volk and Vulkan-Headers only);
- `scripts/init_submodules.sh`, which CI now uses instead of a recursive checkout;
- notices; options `DQ8_FIBER_BACKEND`, `DQ8_BUILD_WINDOW`, `DQ8_USE_SYSTEM_SDL3`, `DQ8_BUILD_GS_PARALLEL` (OFF on
  MSVC);
- INTERFACE targets `dq8::minicoro` and `dq8::stb`;
- ADR-0005, 0007, 0011 and 0012.

### Design rules every session follows

**Execution core (ADR-0007).**
- **Safe points switch fibers inside the call and return normally; there is never a return cascade.** They are:
  - `ee_checkpoint_due`: charge `cycles × backedge_scale`, yield if due, always return 0;
  - `dispatch_guest_branch`: charge `dispatch`, yield before calling the target if due;
  - kernel calls: `block_current()` / `request_reschedule()`;
  - each fiber's run loop.
- An interrupt is taken only when IE and EIE are set and EXL/ERL are clear.
- Interrupt handlers run on one handler fiber with `$ra` = 0 and `$sp` = `hle_handler_stack_top`. They never nest.
- The "callee returned with pc == target" rule (`ee_runtime.cpp`) becomes an explicit per-fiber `unwinding` flag.
- A stop on a fiber records the `StopInfo` and switches away for good. `GuestStop` is caught only inside the fiber that
  threw it, and nothing switches fibers inside a `catch` block.
- `cop0_count` is updated at safe points.

**Time.**
- Nothing guest-visible completes instantly: DMA, RPC and CDVD completions are events at `now + CostModel` time.
- Devices call `note_poll` on reads of volatile registers and `note_activity` on writes.
- Only the machine jumps the clock: on idle, on spins, or on a vblank deadline.

**Kernel calls.** The resolution order is a SetSyscall guest handler, then the native service, then a **stop**.
- Noops are registered explicitly with a note (`kernel_service_noop`).
- Registration is split by owner:
  - **A**: threads, semaphores, event flags, alarms, interrupt handlers, system (SetupThread/Heap, EndOfHeap,
    GetMemorySize, OSD config, SetSyscall, Copy, FindAddress, GetEntryAddress, the cache and TLB noops, Deci2Call →
    −1, the exit-path calls as stops);
  - **B**: GsSetCrt, GsGetIMR/GsPutIMR and their i-forms, SetVSyncFlag, GetGsDxDyOffset;
  - **C**: sceSifDmaStat, sceSifSetDma, sceSifSetDChain, sceSifSetReg, sceSifGetReg, sceSifStopDma, and their
    i-forms.

**Devices.** The host attaches `make_ee_devices(...)` and then `Iop`.

| Physical range | Owner | Device |
|---|---|---|
| `0x10000000`–`0x10001840` | B | Timers |
| `0x10003000`, `0x10003800`, `0x10003C00` | B | GIF, VIF0 and VIF1 registers |
| `0x10004000`, `0x10005000`, `0x10006000` | B | FIFOs |
| `0x10008000`–`0x1000D400` | B | DMA channels |
| `0x1000E000`–`0x1000E060` | B | DMAC registers |
| `0x1000F000`/`F010` | B | INTC, forwarded to `InterruptController` |
| `0x1000F180` | B | SIO TTY |
| `0x1000F520`/`F590` | B | D_ENABLER |
| `0x11000000`–`0x1100FFFF` | B | VU memory |
| `0x12000000` + 8 KiB | B | GS privileged registers |
| `0x1000F200`–`0x1000F260` | C | SIF registers |
| `0x1C000000` + 2 MiB | C | IOP RAM |

- IPU and the BIOS range stay unhandled (logged).
- B's DMAC stores the SIF channel registers but leaves SIF transfers to C, which performs them through the `sceSif*`
  kernel calls and raises `DmaChannel::sif0` through the `InterruptController`.
- Vblank phases:
  - **gs (B):** toggle CSR FIELD, set VSINT, raise INTC 0 if IMR allows, push `VsyncInfo` to the sink;
  - **kernel (B):** SetVSyncFlag targets;
  - **iop (C):** the pad frame from `IInputSource`;
  - **intc (B):** raise INTC 2 on vblank start and INTC 3 on vblank end.

  A's machine schedules the vblank events from `VideoTiming` and calls the listeners.
- **GuestMemory (B)** gains `set_mmio_bus(const MmioBus*, VirtualClock*, Cycles access_cost)`; A's machine calls it.
  Handled accesses charge `mmio_access`, while unhandled ones keep M2's log and `--trap-mmio`.

**Legal.**
- Upstream PS2Recomp code adapted into our tree gets the license identifier `GPL-3.0-only` in its SPDX header and the line
  "Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors".
- ps2sdk is AFL-2.0 (GPL-incompatible): use it as a protocol reference only, never copy code. Upstream's
  ps2sdk-named structs (`t_SifRpc*` in `Helpers/State.h`) are re-derived as our own offset tables with our own names.
- PCSX2 (GPL-3.0-or-later) may be read and adapted with attribution.
- Game evidence (Ghidra, PCSX2 logs, save states) stays local; record only addresses, ids, offsets and counts.

## Contract decisions after day 0

Recorded in the order they were made. Each names the owner who implements it.

1. **GS backend factories (2026-10-21).**
   - B adds `src/runtime/gs/gs_backends.h` with `std::unique_ptr<IGsBackend> make_null_gs_backend();` and
     `std::unique_ptr<IGsBackend> make_trace_gs_backend(std::filesystem::path file);` (an empty path records nothing;
     the stream hash stays in `GsPipeline`).
   - D adds, in `src/runtime/gs/gs_parallel_backend.h`, `struct ParallelGsConfig { std::filesystem::path library;
     std::uint32_t super_sampling = 1; std::filesystem::path pipeline_cache; };` and
     `std::unique_ptr<IGsBackend> make_parallel_gs_backend(const ParallelGsConfig&);`. It throws
     `std::runtime_error` when the library or the device fails, and the host maps that to exit 3.
   - The parallel backend's loader only `dlopen`s, so it is compiled into `dq8_runtime` on every platform. The
     shared library is the target `dq8_gs_parallel`, with output name `dq8-gs-parallel` plus the platform prefix and
     suffix, built when `DQ8_BUILD_GS_PARALLEL` is on. The host looks for it next to its executable unless told
     otherwise.
   - The SDL3 platform is the static library `dq8_platform_sdl`, built when `DQ8_BUILD_WINDOW` is on. The host
     defines `DQ8_HAVE_WINDOW` when it links it.
2. **Stale-return check (2026-10-21).**
   - E adds `Dispatcher::window_generation()` and `Dispatcher::check_return(std::uint32_t caller_pc,
     std::uint64_t generation_at_call) const`, which stops when `caller_pc` is in the window and the generation has
     moved.
   - A calls them in `dispatch_guest_branch` in debug builds (`!NDEBUG`): read the generation before each dispatched
     call whose `source_pc` is in the window, and call `check_return` after it.
3. **Trace categories (2026-10-21, on `main`).**
   - `machine_services.h` has `TraceCategory {kernel, sif, rpc, dma, gs}`, `trace_bit()`, the pure virtual
     `tracing(category)` and the helper `trace(category, message)`, which emits a `"trace"` event with fields `cat`
     and `msg`.
   - `MachineConfig::trace_mask` holds the bits. A implements `Machine::tracing`; `FakeMachine` has a `trace_mask`
     member, off by default.
   - The host prints trace events on stderr and leaves them out of `--events` unless asked.
4. **Host-requested stops (2026-10-21).**
   - A appends `StopReason::requested` (spelled `"requested"`) for `--stop-at-pc` and `--stop-at-rpc`.
   - `--movies stop` uses `unsupported`.
5. **IOP events (2026-10-21).** C emits:
   - `iop_module` {`name` (string), `id` (u64)} on each module load;
   - `rpc_bind` {`sid` (u64), `server` (u64, 0 while unregistered)} when a bind *succeeds*; failed retries are not
     events;
   - `rpc_call` {`sid` (u64), `fno` (u64)} only when `tracing(TraceCategory::rpc)` is on.

   `.dq8in` `after rpc_bind:SID` and `--stop-at-rpc SID[:FNO]` key on these fields.
6. **The stopped thread's context (2026-10-21).**
   - A appends `std::int32_t thread_id` to `StopInfo` (0 = scheduler stack, negative = the handler fiber).
   - After a stop, `EeRuntime::context()` returns the context of the thread (or handler) that was running when the
     run stopped. That is what the host's JSON `context` prints.

## Upstream reuse (PS2Recomp `eb8688b`)

| Upstream | Destination | Change |
|---|---|---|
| `ps2xRuntime/src/lib/Kernel/EeScheduler.cpp`, kernel-object core (about 900 of 2,146 lines) | A: `src/runtime/kernel/{scheduler,kernel_objects}.cpp` | The 5 `throw EeDispatcherTransfer` sites become `block_current`/yield. No host threads, mutexes or `steady_clock`. Alarms tick on hblanks. Vblank at 60000/1001. No interrupt stacks carved at `0x01F00000` |
| `Kernel/Syscalls/{Thread,Sync,Interrupt}.cpp` (about 1,000) | A: `kernel/services_*.cpp` | Re-signed to `KernelCallContext`; unimplemented paths stop instead of returning 0 |
| `Kernel/Syscalls/System.cpp` (selected functions) | A: `kernel/services_system.cpp` | Each verified against PCSX2 register state |
| `Helpers/State.h:21-137` (`KE_*`, thread/sema parameter layouts) | A: `kernel/kernel_abi.h` | SIF structs excluded (see Legal) |
| EE timer block in `ps2_memory.cpp` (about 190) | B: `machine/ee_timers.cpp` | Counts lazily from the clock; compares and overflows as events |
| DMA chain walker in `ps2_memory.cpp` (about 250) | B: `dma/dmac.cpp` | Deferred completion; SPR targets |
| `ps2_vif1_interpreter.cpp` (783) | B: `dma/vif.cpp` | A `Vif` class over VU memory and the GIF; a VU0 instance |
| `vu/ps2_vu1_{core,lower,upper}.cpp`, `ps2_vu1.h` (about 3,500) | B: `vu/vu1_*.cpp` | `GS&`/`PS2Memory*` become an `IVuHost` (XGKICK, memory, invalidate) |
| `ps2xIOP/src/modules/mcserv.cpp` (404) | C: `iop/services/mcserv.cpp` | Onto `IopRpcServer` |
| `ps2xTest/src/ps2_runtime_kernel_tests.cpp` (32 cases), `ps2_runtime_interrupt_tests.cpp` (10), `ps2_memory_tests.cpp` (53), `ps2_vu1_tests.cpp` (40) | A, B: Catch2 tests | Expected values ported; the harness is rewritten |

**Not reused:**
- the by-name library fakes in `Kernel/Stubs/*` (those libraries are game code here);
- the GS frontend and CPU rasterizer (at most a test oracle);
- the `ps2xIOP` interpreter (M4);
- silent-0 syscalls; the 60.000 Hz vblank.

## Sessions

Each session works in its own worktree, branched from `main` after day 0, and touches only the files it owns.
- If you need a contract change, ask the coordinator; do not edit another session's files.
- Write SPDX headers. Code must be clang-format clean (format with `pre-commit run clang-format --files ...` after
  staging) and warning-free on GCC and Clang.
- **Tests are synthetic only**: no game data and no GPU in CI.
- Before reporting:
  - `cmake --build --preset dev-clang`, `ctest --preset unit-dev-clang`, the `asan` preset,
    `python3 -I scripts/check_no_game_data.py --tree --deep`, `pre-commit run --all-files`, `reuse lint`;
  - merge `main` into your branch;
  - list what you could not verify.

### A: Execution core and kernel HLE (largest)

**Owns:**
- `src/runtime/kernel/**`, except `kernel_services.*` and `kernel_layout.h` from day 0, which A may extend with
  the coordinator;
- `src/runtime/machine/machine.cpp` (and private helpers under `machine/` that B does not own);
- `src/runtime/ee/{ee_runtime,guest_stop,context}.*`;
- tests `test_{fiber,scheduler,kernel_threads,kernel_sync,kernel_interrupts,kernel_system,kernel_calls,ee_runtime_contract}.cpp`;
- `tests/unit/support/contract_unit.*`.

**Delivers:**
- `rt::Fiber`: minicoro backend with guard pages and a headroom check; `ucontext` and `win32` backends at least
  compile.
- `Scheduler`, `Machine`, and the per-thread `R5900Context`.
- The four safe points and the handler fiber.
- The `unwinding` flag that replaces the pc-unchanged rule.
- Stops as fiber switches. `KernelHle` is removed, and `EeRuntime` uses `KernelServices`.
- Services for every name and number in the "Kernel calls" ownership list above, the kernel RAM sentinel, the
  deadlock report and the no-progress watchdog.
- M2's `--stop-at-first-syscall` behaviour through `MachineConfig::stop_at_first_syscall`.

**Done when:**
- The ported kernel and interrupt suites pass.
- The contract unit runs:
  - two threads with a semaphore;
  - a vblank handler that `iSignalSema`s;
  - an interrupt taken at a back-edge checkpoint inside a guest loop with no return cascade.
- Regression cases pass: a self-entry back-edge with a forced safe point, and a `jr` to the callee's own entry.
- Fiber tests pass under ASan: deep recursion to a clean stop, and throw/catch inside a fiber.
- Running the same program twice gives the same event trace. Every M2 contract case still passes.

### B: Devices, DMA, VIF, GIF, VU1, GS front end, trace and null backends

**Owns:**
- `src/runtime/ee/guest_memory.*`;
- `src/runtime/machine/{intc_regs,ee_timers,ee_devices}.*`;
- `src/runtime/dma/**`, `src/runtime/vu/**`;
- `src/runtime/gs/**` except `gs/parallel/` and `gs/gs_parallel_backend.*`;
- tests `test_{guest_memory,mmio_devices,dmac,vif,gif,vu1,ee_timers,gs_priv,gs_trace,gs_pipeline}.cpp`.

**Delivers:**
- The device map and the vblank phases above, `GuestMemory::set_mmio_bus`, and `make_ee_devices`.
- DMAC (normal and chain; all tag ids, ASR, TTE, IRQ; deferred completion); VIF0/1; GIF packet cutting with a
  SIGNAL/FINISH/LABEL/TRXDIR scan.
- VU1 interpreter with XGKICK to PATH1. VU0 micro mode only if it is cheap; otherwise a diagnostic stop.
- The GS privileged page and the GS kernel calls (SetGsCrt writes the NTSC SMODE/SYNC values from PCSX2's GPL
  source, with attribution).
- `GsPipeline` (SPSC queue, render thread, mailbox, producer-side SHA-1 chain, PNG dumps with stb).
- `null` and `trace` backends. The trace format is PCSX2's dump packet body with no header:
  - 0 = transfer {u8 path, u32 size, data}
  - 1 = vsync {u8 field}
  - 2 = read FIFO {u32 size}
  - 3 = privileged registers {8,192 bytes}

  It must stay readable by S5's replayer (`parallel-gs-replayer --raw`).

**Done when:**
- The ported memory and VU1 suites pass.
- Synthetic chains cover every tag; UNPACK formats and masks are tested.
- MPG + MSCAL of a tiny VU1 program XGKICKs a packet that matches a golden trace hash.
- CSR FIELD/VSINT/IMR, timer compare on all four clock sources, the trace round-trip, and unhandled-register
  log/trap all pass.

### C: SIF, IOP HLE, disc, pad service

**Owns:**
- `src/runtime/iop/**` (including the `sceSif*` kernel services), `src/runtime/io/**` (except E's input sources);
- `tests/unit/support/sif_client.h`;
- tests `test_{sif,iop_rpc,fileio,cdvdfsv,iopheap,loadfile,padman,mcserv,disc_source}.cpp`.

**Delivers:**
- `IopMemory`.
- `SifLink`:
  - registers with boot flags set at reset;
  - the SIF1 path into IOP RAM and the command layer;
  - the SIF0 path (data writes, then the end packet, then `raise_dmac(sif0)`), queued until the EE arms receive.
- System commands: CHANGE_SADDR, SET_SREG, INIT_CMD, RESET (IOPRP: drop modules, keep the standard services, set the
  flags after a latency), RPC BIND/CALL/END/RDATA.
- The RPC layer: an unregistered sid binds as server 0. `IopModuleRegistry` uses the profile's `[iop]` rows.
- Servers:
  - fileio over `IDiscSource` (`cdrom0:` paths, backslashes, `;1`);
  - iopheap; loadfile;
  - cdvdfsv: init, S-commands, N-command Read whose end packet waits for the virtual read, SearchFile, disc ready;
    `0x8000059C` is a stopping stub until it is identified;
  - padman v1 (DMA frame from `IInputSource` at the iop vblank phase);
  - mcserv (adapted, "card present, unformatted");
  - `null:<sid>` servers.
- `make_iso_disc_source`.
- Protocol layouts go in this file under "Protocol notes", in our own words.

**Done when:** a host-side client that speaks the protocol as guest code would can:
- bind fileio, open/lseek/read into an unaligned EE buffer, and get matching bytes from a synthetic ISO
  (`tests/unit/support/iso_builder`);
- SearchFile, then read sectors with the end packet delayed by the cost model;
- load a module whose services become bindable.

Also: an unregistered sid gives server 0, the reset handshake works, and an IOP→EE delivery is queued until armed.

### D: paraLLEl-GS library, parallel backend, SDL3 platform

**Owns:**
- `src/runtime/gs/parallel/**`, `src/runtime/gs/gs_parallel_backend.*`, `src/runtime/platform/sdl/**`;
- `cmake/{ParallelGs,Sdl3}.cmake`, `patches/parallel-gs/`;
- `tools/gs/dq8-gs-replay` (a raw trace or PCSX2 dump through *our* wrapper, to PNG or a window);
- tests `test_gs_parallel_abi.cpp`, plus an SDL dummy-driver smoke test.

**Delivers:**
- `dq8-gs-parallel` (SHARED): wrapper + parallel-gs (`PARALLEL_GS_STANDALONE=ON`) + `granite-vulkan`/`-util`/`-math`/
  `-volk`, hidden visibility, exporting only `dq8_gsp_*`.
  - Headless Granite context; scanout force-progressive with anti-blur, as S5b compared; persisted pipeline cache.
  - Third-party targets get neither our warnings nor sanitizers.
- The parallel `IGsBackend`, which loads the library with `platform/dynlib`.
- `IPlatform` over SDL3:
  - window; keyboard and gamepad mapped to `PadState` with a fixed M3 mapping;
  - `present()` as a texture upload with a 4:3 or 16:9 fit.
- `DQ8_USE_SYSTEM_SDL3`.

**Done when:**
- `nm -D` shows only `dq8_gsp_*`, and create fails cleanly with no device.
- The SDL dummy-driver test passes.
- **Locally:** S7's settings dump (`work/pcsx2/probe-20261019-183359/settings-screen.gs`) replays through our wrapper at
  SSIM ≥ 0.99 against S5b's reference (`work/parallel-gs/ref/settings-rgba.png`), and shows in the window.
- MSVC: OFF by default; record the result of one build attempt.

### E: Game layer, overrides, host, CI, local tooling

**Owns:**
- `src/runtime/ee/{dispatcher,dispatch_table,overlay_manager,overrides}.*`, `src/game/**`, `src/host/**`;
- the input sources in `src/runtime/io/input_*.{h,cpp}`;
- `.github/workflows/ci.yml`;
- `tests/unit/test_{overrides,dispatch,input_script,host_options}.cpp`, `tests/unit/support/machine_unit.*`;
- `tests/local/**`, `tools/bringup/`, `Docs/formats/dq8-input-script.md`;
- optionally `patches/ps2recomp/0001-unhandled-to-signal-exception.patch`.

**Delivers:**
- Overrides: the dispatcher keeps owned table copies and patches override slots to one trampoline that looks up
  `ctx->pc`; `resolve()` keeps its cost. Installing an override checks that the address is a function entry, and
  checks the digest when one is given.
- A debug stale-return check (`OverlayManager::generation`).
- The game layer:
  - the residency hook (ADR-0012);
  - movie skip (`--movies skip|stop`, from `[movie]`);
  - `[iop]` → `IopModuleSpec`;
  - the mode watcher (`mode_variable`), emitting `mode` events;
  - `--until` conditions.
- Input sources: `.dq8in` scripts (absolute vblanks, or relative to `overlay:NAME`, `mode:N`, `rpc_bind:SID`),
  live (latched from `IPlatform` once per vblank), and `--record-input`.
- `dq8-game` redesign:
  - threads: machine (slices, limiter between slices), render (`GsPipeline`), main (`IPlatform`);
  - options: `--headless|--window`, `--gs trace|parallel|null`, `--vblanks`, `--until`, `--dump-frame N:PATH`,
    `--gs-trace`, `--input-script`, `--record-input`, `--speed 1.0|max`, `--ssaa`, `--movies`, `--events FILE|-`,
    `--trace kernel,sif,rpc,dma,gs`, `--strict-kernel`, `--verify-overlays`, `--stop-at-pc`, `--stop-at-rpc`,
    `--dump-ram-at`;
  - exit codes 0, 1 and 2 as before, plus 3 = backend or platform init failed and 4 = `--until` not met;
  - JSON-lines events carrying only deterministic fields;
  - every M2 option keeps working.

**Done when:**
- A synthetic multi-thread program in the emitter's shape (threads, a vblank handler, a GIF DMA sprite, a fileio
  read from a synthetic ISO) runs under `dq8-game --headless --gs trace --vblanks 10` to a golden hash (CTest
  `host.dq8_game_machine_synthetic`).
- Override, input-script and option tests pass.
- One `tests/e2e` case passes: a back-edge loop waiting on a flag that a handler sets, compiled by the real
  `ps2_recomp`.

## Merge order and bring-up

**Merge order:** A → B → C → E (M3a bring-up starts) → D (M3b). All five start on day 0.

**Bring-up** (serial, led by the coordinator, on the real disc; short read-only sessions for single questions).

How unknowns are investigated:
- Ghidra (REA or `tools/ghidra/*.java`).
- PCSX2 through `tools/pcsx2/headless_run.py` (with `--verbose-cdvd`, save states at checkpoints), diffing their RAM
  against `dq8-game --dump-ram-at`.
- Every finding goes into the Bring-up log below.

M3a order:
1. crt0 kernel patches;
2. SIF init and the IOPRP reset;
3. module loads and binds (identify `0x8000059C`);
4. `.HD6` and `.DAT` reads;
5. game-loop init: the sound replies the game checks, the padman v1 frame layout;
6. TITLE through the hook;
7. mode 4: mcserv replies, the settings script, movie-skip return values;
8. the title-scene marker (profile `[title]`) and which handler paces the 30 Hz frame.

M3b: `--gs parallel` frames, the window, live input.

## Acceptance

**M3a**
1. `dq8-game --iso … --headless --gs trace --input-script tests/local/input/SLUS_21207-boot-to-title.dq8in
   --until title --vblanks 6000` exits 0. The events show TITLE resident through the hook, mode 4, and no stop.
2. Zero unimplemented-service stops. Every census name and number has a service or a documented noop (coverage table
   below). Unhandled MMIO on the boot path is limited to a reviewed allowlist.
3. **Determinism:**
   - two runs give identical `gs_hash` sequences, final RAM SHA-1 and events;
   - `--speed 1` and `--speed max` match;
   - the `null` and `trace` backends give the same RAM hash.
4. Our disc-read order matches PCSX2's CDVD log up to the title, and the LSN cache holds the six overlay extents.
5. The GS privileged registers at the settings screen equal the PCSX2 dump's.
6. The title's GS signature matches S7: 30 Hz alternation, about 507 kicks per frame, one 256×224 upload per frame.

**M3b**

7. Settings-screen frame through `--gs parallel`: SSIM ≥ 0.98 against the local PCSX2 reference (S5b method: bilinear
   to 640×480, ffmpeg `ssim`).
8. Title frame: SSIM ≥ 0.95 on a logo/text crop. Full-frame SSIM is recorded only, because the background moves.
9. `--window` shows the settings screen and the title at 30 Hz, driven by keyboard and gamepad.
10. `dq8-gs-parallel` exports only `dq8_gsp_*`, ships its LGPL notice, and builds on Linux GCC and Clang. MSVC status
    is recorded.

**Always**

11. CI green: GCC, Clang, ASan and MSVC (GS backend OFF on MSVC), the synthetic machine CTest, `recomp-e2e`. Checker
    `--tree --deep`, `pre-commit` and `reuse lint` clean. M2's `--stop-at-first-syscall` still passes.
12. Performance is recorded, not gated: vblanks per second, time to title, checkpoint overhead.

**Stretch:** New Game makes the next overlay resident through the hook (which overlay it is, PCSX2's CDVD log settles).

**Out of scope:**
- audio; movie playback;
- saves beyond "card present, unformatted";
- VU0 micro mode unless the boot reaches it;
- the presentation pipeline of architecture §6.3; the launcher.

## Protocol notes

*(Session C, in our own words: SIF register semantics, command packet header, system commands, RPC packets, the
fileio and cdvdfsv request and reply layouts as the game uses them.)*

## Bring-up log

| Unknown | Local evidence | Resolution (addresses, ids, offsets, chosen values) | Change |
|---|---|---|---|

## Kernel coverage

*(Filled in at the end of M3a: every census name and number, its service or noop, its owner, and its call count on the
boot-to-title run.)*

## Results

*(Filled in at acceptance: counts, vblanks, SSIM, timings; addresses only.)*
