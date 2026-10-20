# ADR-0008: The game module is built on the user's machine from their disc

| | |
|---|---|
| **Status** | Proposed (evidence complete: spike S1, 2026-10-19); awaiting the project owner |
| **Related** | project-brief.md §9 and §14 (D3), architecture.md §2 and §3, tech-stack.md §6 |

## Context

Nothing derived from the disc may be distributed (project-brief.md §9), so the recompiled game code (the *game
module*) cannot ship; it must be produced where the disc is. The cost of that first-run build was the open question.
Spike S1 measured it on a 6-core Ryzen 5 5600: recompiling the boot ELF takes 2 s and 0.6 GiB; compiling the 8,335
generated units costs about 15,000–16,000 CPU-s (30–40 minutes at 12 jobs), about 6,000 CPU-s (≈15 minutes) with a
precompiled header of the runtime headers; peak RAM 5–7 GiB at 12 jobs (0.35 GiB per typical job, 2–3 GiB for the two
largest units); about 0.5 GB of disk. The generated code needs only a C++20 compiler, CMake and Ninja; both system
compilers compile it unpatched. Windows is unmeasured.

## Decision

- The game module is **built locally by first-run setup** (architecture.md §3) and never distributed. The host
  loads it through the versioned C ABI (ADR-0002).
- **Linux:** use the system compiler (tech-stack.md §6.3 option A); the launcher checks for it and shows instructions.
- **Windows:** plan the bundled toolchain (option B) and validate it on a Windows runner before M5.
- The build uses a precompiled header, `-O2` for function units and `-O0/-O1` for the dispatch table, a job count
  chosen from free RAM, a compiler cache keyed on the deterministic recompiler output, and incremental rebuilds; the
  emitter or the map splits multi-megabyte single-function units.

## Consequences

- First run costs 15–40 minutes of compile on a mid-range machine; the launcher must show progress and allow
  cancel and resume.
- The host release does not force a module rebuild unless the ABI version changes.
- Disk is not a concern (about 0.5 GB); RAM is: the launcher must not start 12 jobs on an 8 GiB machine.
