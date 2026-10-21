# ADR-0007: One deterministic machine thread with guest threads on fibers; separate render, audio and I/O threads

| | |
|---|---|
| **Status** | **Accepted** 2026-10-21 by the project owner, at the start of milestone M3 |
| **Related** | architecture.md §5.5, §12, §13.3, tech-stack.md §5, Docs/milestones/M3-boots.md |

## Context

Recompiled EE code cannot be interrupted at an arbitrary instruction, and a blocking kernel call (WaitSema,
SleepThread) happens in the middle of a native call stack that the guest expects to resume. M2 ran one guest thread
and ended a run by throwing a C++ exception through the generated code. M3 needs many guest threads, interrupt
handlers, a clock and devices, and every test and replay feature needs the same run to produce the same result.

The emitter (PS2Recomp `eb8688b`) already gives us the hooks: `eeCheckpointDue()` on every backward branch (the host
receives a fixed 32), every `jal`/`jalr`/`jr` through `dispatchGuestBranch`, syscalls and by-name kernel calls as host
calls that continue inline. Upstream PS2Recomp blocks by throwing to unwind to its scheduler and re-entering by
address; that works without fibers but cannot suspend in the middle of a host service and makes every preemption a
return cascade.

## Decision

- **One machine thread** runs EE guest code, the attached devices (DMA, VIF, GIF, VU, GS registers, timers, the native
  IOP services), the event queue and the **virtual clock** (EE cycles at 294.912 MHz). Wall-clock time never reaches
  guest-visible state; the limiter waits between machine slices.
- **Each guest thread runs on its own fiber** with its own native stack. A blocking kernel call is a switch to the
  scheduler; when the thread is woken, the call returns normally and generated code continues. Interrupt, DMAC and
  alarm handlers run on one handler fiber (EE handlers never nest).
- **Safe points** are the only places a switch or an interrupt happens: `eeCheckpointDue` (which switches inside the
  call and always returns 0), dispatched branches, kernel calls, and the run loop. No return cascade is ever used for
  preemption.
- Fibers sit behind `rt::Fiber` (`src/runtime/kernel/fiber.h`) with **minicoro** (MIT-0 or Unlicense) as the default
  backend: it annotates fiber switches for AddressSanitizer itself and saves the Windows thread-information block, so
  the ASan and MSVC CI jobs keep working. `ucontext` and Win32 fibers are fallback backends (`DQ8_FIBER_BACKEND`).
- Guest-visible timing comes from a named **cost model** (`src/runtime/machine/cost_model.h`, "m3-v1") charged at
  safe points; any change to it is recorded in the milestone document. Events run in (time, scheduling order) order.
- The **render thread** owns the GS backend (paraLLEl-GS requires all calls from one thread); the **main thread**
  owns the window and input (ADR-0005); audio and disc I/O get their own threads when they arrive (M4).

## Consequences

- A run is a pure function of the disc, the build, the cost model and the input script: tests compare GS-stream
  hashes, RAM hashes and event logs between runs and between `--speed 1` and `--speed max`.
- `GuestStop` no longer unwinds through generated code: a stop on a fiber switches away and the fiber is destroyed
  suspended, which closes the M2 deviation from architecture.md §13.3 for this game. Host frames on guest fibers must
  therefore be abandonable, and nothing may switch fibers while a C++ exception is in flight.
- Each guest thread costs a reserved native stack (8 MiB of address space, 32 MiB under sanitizers).
- Parallelism beyond the render thread (IOP on its own thread, parallel VU work) is added only after profiling and
  only with explicit synchronisation points that keep runs deterministic.
