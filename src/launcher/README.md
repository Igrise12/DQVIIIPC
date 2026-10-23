# src/launcher/

The GUI launcher (`dq8-launcher`): first-run wizard, verification and setup with progress, Play, settings,
controller mapping, diagnostics, About. Dear ImGui over SDL3 (ADR-0010). See Docs/architecture.md §11 and
Docs/milestones/M5-launcher.md.

Rules:
- Links `libdq8core`, Dear ImGui, SDL3 and nlohmann/json only; never the runtime. It starts `dq8-game` and
  `dq8ctl` as separate processes (core's `run_process`, no shell) and reads `dq8-game --status -` with nlohmann/json.
- Settings pages are generated from `config/settings/schema.toml`; a new setting needs no launcher code.
- UI logic lives in view-models that unit tests reach without a window; rendering stays thin.
- No game-derived content: no text, images or fonts from the disc. The font is Noto Sans (SIL OFL 1.1,
  `data/fonts/`), compiled in by `embed_file.cmake`.

Layout:
- `model/` (`dq8_launcher_model`, no SDL or Dear ImGui): the view-models and their backends.
  - `wizard` (the first-run state machine over a `WizardBackend`), `setup_progress` (per-stage rows from
    `setup::Event`), `game_session` (Play: `GameSession` from the status lines, `GameSupervisor` running dq8-game),
    `status` (the status-line parser and a line splitter), `settings_model` (pages and rows from the schema over a
    `SettingsBackend`), `controllers` (profiles, devices, live capture and the test view over a `ProfileStore`),
    `library` (the registry's builds), `diagnostics` (the support bundle: `dq8/core/diagnostics.h`), `markdown`
    (THIRD_PARTY_NOTICES.md for About), `launcher_state` (launcher.json: the last image, page, advanced toggle),
    `environment` (finding dq8-game, dq8ctl and shipped files), `errors` (codes by name, `error_text()`).
  - `core_backends`: the backends over libdq8core. `preview`: stand-ins for `--preview`.
  - Slow work runs on an `Executor` (a worker thread; inline in tests) and reports through a `Mailbox` that the
    UI thread drains once a frame, so view-model state is only touched on the UI thread.
- `ui/`: the `App` (sidebar, page routing, native file dialogs, gamepads, live capture) and one `page_*.cpp` per
  page; `widgets` holds the shared drawing helpers and the style; `sdl_input` turns SDL events into SDL's names.
- `main.cpp`: options, the window, DPI scale (SDL's display scale over the pixel density, re-applied when it
  changes), the font, the frame loop (it waits for events when idle).

Options: `--page NAME`, `--game FILE`, `--dq8ctl FILE`, `--preview` (every page over stand-ins: no disc, no
toolchain, no game, nothing written), `--frames N` (smoke tests on `SDL_VIDEO_DRIVER=dummy`; with `--preview` the
page's work starts by itself), `--screenshot FILE` (with `--frames`, the last frame as BMP), `--version`.

State (M5 session B, merged onto A, C and D on 2026-10-23): every page runs over libdq8core: the wizard over
`setup::run()` and the toolchain check, Play over the registry and dq8-game's status channel, Settings over the
settings store, Diagnostics over `core::diagnostics::write_report()`, and Controllers over session C's profile files
(`FileProfileStore`, `user_config_dir()/input/`; in memory under `--preview`). The Windows toolchain download's
consent dialog arrives with session E.
