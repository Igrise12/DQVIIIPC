# ADR-0019: VU1 programs compiled ahead of time into a locally built companion library

| | |
|---|---|
| **Status** | **Proposed** 2026-10-24. The owner reviewed the plan (Docs/milestones/M6-vu1-aot.md, "Owner decisions") the same day; acceptance waits for phase P0's measurements and the checkpoint after them. Not implemented |
| **Related** | architecture.md §2.1, §5.4, §12, §13.3, §14, §15; ADR-0002, ADR-0007, ADR-0008, ADR-0017, ADR-0018; Docs/milestones/M4-playable.md (bring-up log, "Where the field's time goes") |

## Context

The field and the in-engine scenes run at 16–20 % of real time on the dev box (2026-10-24, release preset). The
machine thread is pinned at one core, and most of it goes to the VU1 interpreter. That interpreter models the
pipelines cycle by cycle, at roughly 300 host cycles per VU cycle (*inferred*). M4 already made it 2.6x faster
without changing behaviour. Further gains of that kind will not give the 7–17x the field needs.

Architecture §5.4 plans the answer: a census of the programs the game uploads, a compiler in `tools/vu1/`, compiled
programs matched by content at run time, and the interpreter as fallback. Several points are still open:

- where the compiled code lives;
- what it must preserve;
- what happens when it cannot continue;
- how it is kept in step with the interpreter;
- how it gets onto a user's machine without game data leaving it.

Facts that shape the answer (M6-vu1-aot.md, "What exploration established"):

- VU1 runs synchronously inside the VIF command that starts it, and its cycles never reach the machine clock.
- Every program starts with empty pipelines.
- Values inside a program depend on timing: Q and P, the flag pipelines, the VI branch rule, loads against pending
  stores, and the PATH1 copy against store commits.
- A program jumps into code uploaded separately, so a program is an entry pc plus its reachable code, not an upload.

## Proposal

1. **The interpreter is the reference.** A compiled program must end with the same `VuState`, VU1 data memory, PATH1
   packets (bytes and order), cycle count and stop (if any) as the interpreter, from any entry state. Anything the
   compiled code does not model exactly is a **bail-out**: at an instruction-pair boundary it hands a resume record
   (pending writes, flags, FDIV and EFU, stores, the PATH1 cursor) to the interpreter, which finishes the program.
   Correctness never depends on the compiled set.
2. **One source for the rules.** Decode, latencies and FMAC arithmetic are shared: one ISA library and one
   arithmetic header, used by both the interpreter and the generated code. The compiler takes its timing from the
   interpreter itself, by running each block on it with placeholder values. Every library records a digest of the ISA
   tables and the arithmetic version, and the host refuses a library built against another.
3. **A companion library.** The compiled programs form one shared library per profile, `dq8_vu1_<profile>`. It sits
   beside the EE game modules, behind its own small versioned C ABI (`include/dq8/abi/vu1_program.h`). The user's
   toolchain builds it on the user's machine (ADR-0008; llvm-mingw on Windows, ADR-0018) with `-ffp-contract=off` and
   no fast-math. It is never distributed. Rebuilding it ("Optimize") touches no game module.
4. **Matching by content.** Each program is keyed by (unit, entry pc, closure). The runtime compares a candidate's
   closure bytes with micro memory once per micro-memory generation and pc. A match runs compiled code; a miss runs
   the interpreter and is recorded in the census.
5. **The census is local data.** It lives in the user data directory (contributors: `work/`) and holds the closure
   bytes, counters and observed indirect targets. It is written on the I/O thread and never feeds the guest, the
   events or the `--json` report. It is never committed, and the checker refuses it.
6. **Verified three ways.** In CI, a synthetic corpus and the snapshot round trip, plus the committed differential
   fuzzer with seeded faults. Locally, a lockstep `--vu1 verify` mode on the disc and byte-identical reports between
   `--vu1 interp` and `aot`. On the user's machine, a self-test before a library is registered.

## Consequences

- Each new program costs the first play: a scene with programs not yet compiled runs on the interpreter until the
  user optimizes. The owner chose (2026-10-24) to add a published, address-only program list, read from the user's
  disc at setup, if P0 shows the programs sit at fixed places on the disc. It gets its own ADR, as ADR-0017 did for
  maps. Hot-loading a new library at a program boundary, which the contract would make safe, is not planned.
- Changing the interpreter's timing or arithmetic changes the ISA digest and makes existing libraries stale. That is
  an event, like a game-module ABI bump: the launcher offers Optimize, and the interpreter runs meanwhile.
- `dq8-vu1c` ships with the host, as `ps2_recomp` does. The launcher and `dq8ctl` run it through `libdq8core` and
  never link the runtime (architecture §13.2); the self-test runs as a `dq8-game` process.
- VU0 micro programs stay on the interpreter. The compiler could take them later, because the interpreter is shared.
- The cycle count is preserved although nothing reads VU1 time today. If VU1 time is ever charged to the machine
  clock, compiled programs already agree.

## Alternatives considered

- **Compile into the EE game modules.** One library fewer, but every Optimize would relink a game module, and an EE
  rebuild would throw away compiled VU1 code. Rejected.
- **A JIT** (architecture §5.4, stage 3). No system compiler is needed at run time, and programs compile on first
  sight. But it is a large new component with per-platform code generation, and harder to audit. It stays the
  fallback if gate G1 in the plan fails.
- **Approximate timing** (execute pairs in order, no pipelines). Faster and simpler, but values that depend on timing
  would differ from the interpreter, so no golden could prove equivalence. Rejected unless the owner relaxes the
  contract.
- **A VU1 thread (MTVU) or VU1 on the GPU.** The first overlaps only the work that is not VU1 and needs sync points at
  every observation to stay deterministic. The second suits these short, serial, branchy programs poorly. Neither
  replaces compilation.
