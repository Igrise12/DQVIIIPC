# src/launcher/

The GUI launcher (`dq8-launcher`): first-run wizard, verification and setup with progress, Play, settings,
controller mapping, diagnostics, About. Dear ImGui over SDL3 (ADR-0010). See Docs/architecture.md §11 and
Docs/milestones/M5-launcher.md.

Rules:
- Links `libdq8core`, Dear ImGui, SDL3 and nlohmann/json only; never the runtime. It starts `dq8-game` and
  `dq8ctl` as separate processes and reads their JSON-line events (`--status`, the setup's stage events).
- Settings pages are generated from `config/settings/schema.toml`; a new setting needs no launcher code.
- UI logic lives in view-models that unit tests reach without a window; rendering stays thin.
- No game-derived content: no text, images or fonts from the disc.

State: M5 day 0 is a placeholder window (`main.cpp`; `--frames N` closes it after N frames for smoke tests).
M5 session B builds the launcher here.
