# ADR-0003: Use PS2Recomp as a pinned dependency with a patch queue; write our own runtime

| | |
|---|---|
| **Status** | **Accepted** 2026-10-20 by the project owner (evidence: spikes S1 and S2, 2026-10-19) |
| **Related** | project-brief.md §14 (D4), architecture.md §4.7, tech-stack.md §5 |

## Context

PS2Recomp (ran-j, GPL-3.0) is the only general PS2 static recompiler with an active ecosystem. Its runtime has
partial hardware coverage and no notion of code overlays, which this game relies on (six `BIN/*.BIN` units sharing
one address window). We need control of the runtime, but rewriting the recompiler would cost months.

Spike S1 built commit `eb8688b2fd191129f103a39543002c005b33409c` (2026-10-14) with GCC 16.2.1 and Clang 23.1.1 on
Linux with **zero patches**; all 445 upstream tests pass; upstream CI already covers Linux GCC and Clang. Configure
pulls nine repositories through CMake FetchContent, and an offline configure with `FETCHCONTENT_FULLY_DISCONNECTED`
plus `FETCHCONTENT_SOURCE_DIR_<NAME>` overrides works today. The recompiler is deterministic. The changes we will
certainly need are small and local to `ps2xRecomp`: one address space per run (per-overlay synthetic ELFs), per-unit
prefixes for the emitted dispatch table, and no `throw` on unhandled instructions. Spike S2 showed PS2Recomp's
Ghidra exporter runs headless unmodified with a `.properties` answer file.

## Decision

- Pin PS2Recomp at `eb8688b` as a git submodule under `third_party/` and keep its nine FetchContent dependencies as
  pinned submodules, configured offline.
- Keep an ordered patch queue under `patches/ps2recomp/` (empty today); every patch is small, documented, and sent
  upstream when it is general. Re-evaluate forking only if the per-code-unit changes grow past a few hundred lines.
- Use `ps2xRecomp` (and the Ghidra exporter) as the recompiler; write our own runtime (architecture.md §5–§12) rather
  than extending `ps2xRuntime`, borrowing from it with attribution where useful.

## Consequences

- The combined work is GPL-3.0 (ADR-0009). Upstream's `ps2xRuntime` dependencies (raylib, ImGui, FFmpeg) are not ours.
- Our setup pipeline builds `ps2_recomp` itself (60–80 s, developer and CI cost) and ships it with the host.
- The function map, not the analyzer's JAL scanner, defines function boundaries (`ghidra_output` makes map ranges
  authoritative); the map stays local for now (D9).
