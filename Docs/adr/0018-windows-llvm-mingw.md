# ADR-0018: On Windows, the host and the user's game modules are both built with llvm-mingw

| | |
|---|---|
| **Status** | **Accepted** 2026-10-22 with the M5 plan (the owner chose Linux and Windows for M5) |
| **Related** | ADR-0002, ADR-0008; tech-stack.md §6.3, §9.2; Docs/milestones/M5-launcher.md |

## Context

The game modules are compiled on the user's machine (ADR-0008) and loaded by the shipped host over a C ABI (ADR-0002).
The ABI passes only plain C, but control does not: host services can end a guest call by throwing a C++ exception
that unwinds through the module's frames back to the host's run loop. On Linux the system compiler builds both sides
with one exception model. On Windows an MSVC-built host and modules from another compiler would not share one.
MSVC itself cannot be redistributed, and the runtime has so far only been built with MSVC in CI (without the GS and
movie libraries) and never run on Windows.

## Decision

- Windows builds of the host (`dq8-game`, `dq8-launcher`, `dq8ctl`, `ps2_recomp`, the GS and movie libraries) are
  **cross-compiled with a pinned llvm-mingw** (clang, mingw-w64, libc++), from Linux in CI.
- Users' game modules are compiled with the **same pinned llvm-mingw**, downloaded on demand after the user agrees,
  checked against a pinned SHA-256, and unpacked into the user data directory; cmake and ninja ship with the program.
- The MSVC CI job stays as a compile check of the code base; it does not produce release builds.
- Windows smoke tests run under Wine on Linux during development; acceptance needs a run on real Windows.

## Consequences

- One toolchain, one C++ runtime and one exception model on both sides of the module boundary.
- Windows releases are built from the same Linux CI as the Linux ones.
- The download is a few hundred megabytes on first setup; the launcher explains it and asks first.
