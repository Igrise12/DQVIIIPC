# ADR-0010: The launcher is a thin front end over `libdq8core`, in Dear ImGui over SDL3

| | |
|---|---|
| **Status** | **Accepted** 2026-10-22 by the project owner (M5 planning; decides brief §14 D6) |
| **Related** | architecture.md §11; tech-stack.md §5; ADR-0005, ADR-0006; Docs/milestones/M5-launcher.md |

## Context

M5's exit is that a new user goes from their disc image to playing without a terminal: a first-run wizard,
verification, setup with progress, settings, controller mapping and resolution scaling. Brief §14 D6 proposed Dear
ImGui over SDL3; the tech-stack review rejected Qt, RmlUi and Tauri as heavier or less suited.

## Decision

- `dq8-launcher` is a separate executable written with **Dear ImGui over SDL3** (SDL's renderer backend, SDL3's native
  file dialogs, an open font bundled with it).
- It is a **thin front end**: setup, verification, toolchain checks, the build registry, settings and diagnostics are
  typed APIs with progress streams in `libdq8core`, shared with `dq8ctl`. The launcher links `core` (plus SDL3 and
  Dear ImGui) and never the runtime.
- It **spawns `dq8-game`** with a settings file and reads a status channel (first frame, frame rate, warnings, exit
  reason) that is separate from the deterministic event log.
- UI state lives in view models that unit tests drive without a window; the drawing code stays thin.

## Consequences

- One small, permissively licensed toolkit that can later serve an in-game overlay too.
- The launcher looks like a tool rather than a native application; a different front end can replace it over the same
  core API.
- Every launcher operation is also available from `dq8ctl` (architecture §11.3).
