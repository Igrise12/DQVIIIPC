# M5: Launcher MVP

| | |
|---|---|
| **Status** | Planned 2026-10-22 with the project owner; day 0 on `main` 2026-10-22; sessions A–E not started |
| **Exit (brief §11)** | First-run wizard, verification, setup with progress, settings UI, controller mapping, resolution scaling: a new user goes from the ISO to playing without a terminal |
| **Owner choices (2026-10-22)** | Function maps published address-only, jump-table entries read from the user's disc (ADR-0017, D9) · launcher in Dear ImGui over SDL3 (ADR-0010, D6) · schema-driven settings (ADR-0006) · Linux **and** Windows, both host and modules built with llvm-mingw on Windows (ADR-0018) · day-0 contracts, five parallel sessions, then integration; full speed stays in M6 |
| **Still open** | M4's town, shop and battle route (B3, B4) is the owner's playtest; ADR-0013 stays a proposal |

Sessions read this file. Contract changes go through the coordinator. Every rule in CLAUDE.md applies; the most
important is that **no game-derived content** goes into commits: published maps hold addresses, sizes and table
shapes only; settings, string tables, fonts and images are our own; diagnostics exports never carry game data.

## What exploration established

**Launcher, settings and features were design only.** `src/launcher/` and `src/features/` held READMEs; there was no
settings schema, no settings file and no `--set`. Architecture §9 (layering, TOML, schema fields), §11 (separate
processes, screens, stable error codes) and §3 (setup stages, structured progress, registration) were unimplemented.
`libdq8core` already had the pieces the setup needs: `ProgressFn` (`intake.h`), the recompile pipeline's
`on_progress` and `cancel` (`recomp/pipeline.h`), `run_process()` with streamed output and cancellation
(`platform/process.h`), the `build-manifest.toml` record and `user_data_dir()`.

**The setup's blocker was the function maps.** `dq8ctl recomp` reads `work/recomp/maps`, which only a contributor
with Ghidra can produce (`tools/ghidra/export_maps.sh`). The maps were also refined by hand during M4 bring-up (MENU
grew from 263 to 418 functions in B2), so regenerating them on a user's machine would not reproduce them. No toolchain
check existed (a missing cmake or compiler surfaced as a failed step) and progress was free text on stderr.

**The contributor maps (counts only).** Seven units, 12,302 functions in all (main 10,501, shop 454, menu 418,
battle 617, casino 224, title 72, viewer 16). 235 of them carry a name other than Ghidra's placeholder patterns
(`FUN_`, `thunk_FUN_`, `sub_` plus the address): kernel and SDK names such as the syscall wrappers. Whether
`ps2_recomp` treats any name specially is for session D to establish before it replaces names.

**dq8-game.** Every setting is a flag (`host/options.h`); the window is a fixed 1280×960 resizable window with vsync on
and no fullscreen; the picture is presented at 4:3; `--ssaa` only anti-aliases: a real higher-resolution picture also
needs `high_resolution_scanout`, `super_sampled_textures` and `backbuffer_promotion`, which `ParallelGsConfig`
(`gs/gs_parallel_backend.h`) has but nothing sets (spike S5b). Bindings are switch tables (`sdl/sdl_mapping.cpp`),
sticks are linear without dead zones, ports follow plug order, audio uses the default device without a volume.

**Widescreen.** The game's own screen-size option (the byte at `0x003D275C`) was on in the M4 runs: the game drew
anamorphic frames and played the `_W` movies while the presenter showed 4:3. That is M4's open "movie geometry" item;
following the byte in the presenter (`display.aspect = auto`) resolves it.

**Packaging.** None: `install()` missed `dq8ctl`, `data/regions`, `cmake/module` and the ABI headers and set no
RPATH; `dq8-movie` links the system's libavcodec (ADR-0015 moved a bundled LGPL build to M5).

**Windows.** CI builds the host with MSVC as a compile check (no GS, no movie library); the runtime has never run on
Windows. Host exceptions unwind through module frames, so host and modules need one toolchain (ADR-0018).

## Design rules every session follows

- **Layering (architecture §13.2).** The launcher and `dq8ctl` link `core` only (the launcher also Dear ImGui, SDL3 and
  nlohmann/json); they start `dq8-game` as a process. Settings, setup, toolchain, registry and diagnostics logic live
  in `core` behind typed APIs with progress callbacks, so `dq8ctl` and the launcher share them.
- **Determinism.** `dq8-game --events` stays deterministic. What the launcher needs from a running game (first frame,
  frame rate, warnings, exit reason) goes on the separate, non-deterministic `--status` channel.
- **Goldens.** `dq8-game` reads the settings store only when `--config` or `--set` is given; without them every run
  behaves exactly as in M4, and the M3/M4 golden hashes and tests hold unchanged. The schema's defaults are M4's
  `--window` defaults, so a default settings file plays as M4 did.
- **Settings change behaviour only through the schema.** A new setting is a schema row, a string-table entry and the
  code that reads it; the launcher needs no change (architecture §17).
- **Host-side only.** Volume, rumble strength, dead zones, aspect, scaling and filtering act on the host side of the
  machine (after the audio hash, before presentation), so they never move a deterministic hash.
- **No game data** in maps (shape only), settings, diagnostics or release artifacts; the checker scans release trees.
- **Errors carry stable codes** (`dq8/core/error.h`) and a message with an action (`dq8/core/error_text.h`).

## Contracts (day 0, on `main`)

| Area | Files | What it fixes |
|---|---|---|
| ADRs | `Docs/adr/0006`, `0010`, `0017`, `0018` | Schema-driven settings; Dear ImGui over SDL3; published address-only maps; llvm-mingw for Windows host and modules |
| Error codes and texts | `include/dq8/core/error.h`, `error_text.h`, `src/core/error_text.cpp` | Nine codes (`settings-parse`, `settings-invalid`, `toolchain-incomplete`, `download-failed`, `insufficient-space`, `setup-cancelled`, `smoke-failed`, `maps-missing`, `registry-parse`); a message and an action for every code (a code without a text fails to compile) |
| Settings | `config/settings/schema.toml`, `config/settings/strings/en.toml`, `include/dq8/core/settings.h`, stub `src/core/settings/settings.cpp` | The schema format and its 19 settings (below); `Schema`, `StringTable`, the layered `Store`, `find_settings_dir`, `default_user_file` |
| Setup | `include/dq8/core/setup.h`, `src/core/setup/setup.cpp` | Stages, the event struct and its JSON line (implemented), `Options` with cancel flag and event sink, `Result`; `run()` is the stub |
| Toolchain | `include/dq8/core/toolchain.h`, stub `src/core/toolchain/toolchain.cpp` | `detect()` (cc, cxx, cmake, ninja, versions, bundled root), `check()` → problems with codes and advice, `default_jobs()` (implemented: the RAM-based job count) |
| Registry | `include/dq8/core/registry.h`, `src/core/registry/registry.cpp` | `installs.toml` in the user data directory: profile, ISO, module directory, manifest SHA-1, build time, host version; `find`/`upsert` implemented, `load`/`save` stubs |
| Paths | `src/core/platform/paths.{h,cpp}` | `user_config_dir()`: `$XDG_CONFIG_HOME/dq8-native` (default `~/.config/dq8-native`), the user data directory on Windows and macOS |
| dq8-game options | `src/host/options.{h,cpp}`, `src/host/run_machine.cpp` | `--config FILE`, `--set KEY=VALUE` (repeatable), `--status FILE\|-` parsed and documented; refused at start-up until session A wires them (`unwired_m5_option`) |
| Launcher | `src/launcher/` | `dq8-launcher`: a placeholder Dear ImGui window (`--frames N` for smoke tests, `--version`) |
| Dependencies | `third_party/imgui` (v1.92.9), `third_party/nlohmann_json` (v3.12.0), `third_party/CMakeLists.txt`, `THIRD_PARTY_NOTICES.md` | `dq8::imgui` (static, SDL3 platform and SDL renderer backends, no demo) and `dq8::nlohmann_json`; `DQ8_BUILD_LAUNCHER` (default on, needs `DQ8_BUILD_WINDOW`) |
| Build | `tests/unit/m5_{core,launcher,display,maps,windows}_tests.cmake` | One test list per session; no session edits a shared CMake list |
| Fixes | `src/host/run_machine.cpp`, `src/core/recomp/module_build.cpp` | The GS library is found through `default_parallel_gs_library()` (not hard-coded next to the executable, which breaks a `bin/` + `lib/` layout); a build failure names the right flag (`--abi-include-dir`) |
| Tests | `tests/unit/test_m5_contracts.cpp`, `m5_core_tests.cmake`, `m5_launcher_tests.cmake` | Error texts, the schema and its strings, the event line, the config directory, the registry lookup, the option parsing and refusal, the launcher on SDL's dummy driver |

### The settings schema (`config/settings/schema.toml`)

`schema_version = 1`, then one `[[setting]]` per setting:

| Field | Meaning |
|---|---|
| `key` | `category.name`, lower case, digits and `_`; added, never renamed or reused |
| `type` | `boolean`, `integer`, `real`, `choice`, `text`, `path` |
| `default` | Inside the range or among the choices; M4's `--window` behaviour |
| `choices` / `min`, `max` | `choice` / `integer`, `real` (inclusive) |
| `label`, `description` | String-table ids, always `<key>.label` and `<key>.description`; a choice's text is `<key>.choices.<value>` |
| `category` | The settings page; its title is `category.<name>` in the string table |
| `advanced`, `requires_restart`, `hot_reload` | Shown only with the page's advanced toggle; applies on the next start; applies live (none in M5) |
| `requires` | `["key=value", ...]`: shown and applied only when all hold (`Setting::conditions` in C++, where `requires` is a keyword) |
| `owner` | The component that reads it: `host`, `runtime.platform`, `runtime.gs` |

String tables are `config/settings/strings/<language>.toml` with one `[strings]` table (English in M5). A user
settings file is TOML with `schema_version` and one table per category (`[display]`, `[audio]`, ...); unknown keys
are kept on save, invalid values are reported (`settings-invalid`) and replaced by the default, never fatal.

**Layers** (architecture §9.1): schema defaults → user file (`user_config_dir()/settings.toml`, or next to the
executable when a `portable` marker file is there) → per-profile overrides (`[profile.SLUS_21207.display]` ...) →
`--set`. The system-wide layer waits for packagers.

### Settings to dq8-game

Explicit command-line flags win over `--set`, which wins over `--config`. Without `--config` and `--set` no setting
applies (the goldens rule above).

| Key | Default | dq8-game behaviour | Session |
|---|---|---|---|
| `display.window_mode` | `windowed` | windowed, borderless fullscreen, exclusive fullscreen; Alt+Enter toggles | C |
| `display.width`, `display.height` | 1280, 960 | the window's size at start (windowed) | C |
| `display.vsync` | true | presentation waits for vblank | C |
| `display.internal_resolution` | `1x` | `1x`…`4x`: super-sampling plus high-resolution scanout, super-sampled textures and back-buffer promotion; `1x` is M4's picture | C |
| `display.aspect` | `auto` | `auto` follows the game's 16:9 option; `4:3`, `16:9`, `stretch` force it | C |
| `display.integer_scaling` | false | whole-number scale factors only | C |
| `display.filter` | `linear` | `linear` or `nearest` scaling | C |
| `audio.output` | `sdl` | `--audio sdl\|null` | A |
| `audio.device` | `""` | the SDL device by name; empty is the default device | C |
| `audio.volume` | 100 | output gain after the audio hash | C |
| `audio.latency_ms` | 64 | `--audio-latency` | A |
| `input.profile` | `default` | the bindings profile (`user_config_dir()/input/<name>.toml`) | C |
| `input.deadzone` | 8 | stick dead zone in percent | C |
| `input.rumble` | true | `--rumble on\|off` | A |
| `input.rumble_strength` | 100 | rumble scale in percent (host side) | C |
| `game.movies` | `auto` | `--movies auto\|play\|skip` | A |
| `game.save_dir` | `""` | `--save-dir`; empty is the default folder in the user data directory | A |
| `game.memory_card2` | `none` | `--memory-card2` | A |

### The status channel (`dq8-game --status FILE|-`)

One JSON object per line, for the launcher; wall-clock based and never part of a golden.

| Line | Fields |
|---|---|
| `{"ev":"started", ...}` | `version`, `profile`, `pid`, `window` (bool) |
| `{"ev":"first_frame", ...}` | `ms` since start |
| `{"ev":"fps", ...}` | `fps`, `speed` (guest time / wall time), once a second |
| `{"ev":"warning", ...}` | `code` (an error-code name or a session-defined kebab name), `message` |
| `{"ev":"exit", ...}` | `status` (the exit status), `reason` (`window-closed`, `until`, `vblanks`, `stop`, `error`), and for errors `code` and `message` |

### The setup's events

`setup::run()` reports stages in this order: `verify`, `extract`, `toolchain`, `maps`, `prepare`, `generate`,
`build`, `smoke`, `register`. Each stage sends `start`, any number of `progress`, then `done`, `skipped` (up to date)
or `error` (with `code`). `dq8ctl setup --events -` prints them, one per line:

```json
{"ev":"stage","stage":"build","state":"progress","done":120,"total":845,"unit":"main","message":"..."}
```

`done` and `total` count the stage's own work (bytes for extract, units for generate, compile steps for build,
taken from ninja's `[n/N]`); `total` 0 means unknown. Cancel (the `cancel` flag) stops child processes, ends with
`setup-cancelled`, and a later run resumes: every stage is idempotent and skips what is up to date.

### Published function maps (`data/maps/<profile>/<unit>.toml`)

Format in [Docs/formats/function-map.md](../formats/function-map.md). In short: the unit's source SHA-1 from the
region profile, each function as `[start, size]`, the link-return entries, each jump table as `{address,
base_register, count}`; names only where they are kernel or SDK names; no value read from the game. The table entries
are read from the user's unit image during `prepare`.

## Sessions

All five start together on the day-0 stubs, each in its own worktree, and touch disjoint files. A file outside a
session's list is changed only through the coordinator. Every session runs dev-clang and its unit tests, asan, the
checker (`--tree --deep`) and pre-commit before it reports, and records what it found under "Protocol notes".

### A: Core APIs, settings and the CLI

- **Settings:** the schema loader and validation, `StringTable`, the layered `Store` (defaults → user → profile →
  `--set`), TOML persistence that keeps `schema_version` and unknown keys, the portable marker; `user_config_dir()`
  use in `find_settings_dir`/`default_user_file`.
- **dq8-game:** `--config` and `--set` applied through a settings-to-options step (the table above, A's rows; C's
  rows arrive as fields C reads) and the `--status` channel (a writer the run loop calls; the window's first frame and
  frame rate come from the existing limiter and present path). Remove `unwired_m5_option`.
- **Setup:** `setup::run()` over the existing intake and recompile code with the stage events, ninja progress, cancel
  and resume; `toolchain::detect()`/`check()` (Linux: actionable advice per distribution family, versions);
  `insufficient-space` check; the registry's `load`/`save`; the smoke test (`dq8-game --headless --until title` with
  the new modules).
- **dq8ctl:** `setup <iso> [--events -] [--jobs N] [--force]`, `settings get|set|list|reset`, `run` (starts dq8-game
  from the registry and the user settings), `diagnose [--out FILE]` (system information, toolchain, settings, logs,
  build manifest; never game data).
- **Owns:** `src/core/{settings,setup,toolchain,registry}/**` and their headers, `src/core/platform/paths.*`,
  `src/cli/` (new command files), `src/host/options.*`, the settings and status parts of `src/host/run_machine.cpp`
  plus new `src/host/settings_wiring.cpp` and `status_channel.{h,cpp}`, `config/settings/**`,
  `tests/unit/m5_core_tests.cmake` and its tests.
- **Done when:** `dq8ctl setup` takes a clean user data directory from the ISO to a registered, smoke-tested build
  (contributor maps until D lands, published maps after); cancel and resume work; `dq8-game --config` reproduces the
  M4 behaviour with a default file and every A row of the table changes behaviour; the status channel reports the
  five lines; settings files round-trip with unknown keys kept.

### B: The launcher

- `dq8-launcher` in Dear ImGui over SDL3: first-run wizard (SDL3's file dialog for the ISO, verification, toolchain
  check with advice, setup with per-stage progress, a log view, cancel and resume, through `setup::run()` on a worker
  thread), Library/Play (starts `dq8-game --iso ... --module-dir ... --config <user file> --status -`, reads the
  status lines, shows exit reasons with `error_text()`'s message and action), Settings (pages generated from the
  schema and string table, `requires` conditions, restart hints, reset), Controllers (profiles per device, live
  capture, a test view; the profile format comes from C), Diagnostics (export through A's API), About (version,
  licences, `THIRD_PARTY_NOTICES.md`).
- DPI-aware (SDL content scale), keyboard and gamepad navigation, a bundled open-licence font (record it in the
  notices).
- UI logic in view-models (wizard state machine, settings page model, process supervisor) testable without a window;
  rendering stays thin.
- **Owns:** `src/launcher/**`, `tests/unit/m5_launcher_tests.cmake`, `tests/unit/test_launcher_*`, a font under
  `data/fonts/` if one is bundled.
- **Done when:** on Linux a user picks the ISO, watches the setup, presses Play and plays, changes settings that
  persist and take effect on the next start, rebinds a pad, and exports diagnostics, without a terminal; the
  view-model tests run in CI.

### C: Display, input, audio and widescreen

- **Display:** window modes (windowed, borderless, exclusive fullscreen; Alt+Enter), start size, vsync; aspect
  `auto`/`4:3`/`16:9`/`stretch` with `auto` following the game's 16:9 option through a game-layer watcher on the
  profile's new `[display] widescreen_variable`; integer scaling; filter; `internal_resolution` driving super-sampling
  and the three high-resolution options of `ParallelGsConfig`, with a pipeline cache in the user data directory.
- **Input:** bindings from per-device profiles (keyboard and gamepad; format `user_config_dir()/input/<name>.toml`,
  documented in `Docs/formats/`), dead zones, stable port assignment by device GUID, hot-plug, rumble strength.
- **Audio:** device selection by name, volume after the audio hash.
- **Owns:** `src/runtime/platform/sdl/**`, the `gs_parallel_backend` configuration wiring, the window and present
  parts of `src/host/run_machine.cpp`, `src/host/audio_wiring.cpp`, new `src/game/display_watcher.*`, the profile's
  `[display]` table (`data/regions/SLUS_21207.toml`, `region_profile.*` for that table only),
  `tests/unit/m5_display_tests.cmake` and its tests.
- **Done when:** each C row of the table changes behaviour on the disc; `auto` shows 16:9 frames and the `_W` movies
  at 16:9 when the game's option is on; `2x` gives a real higher-resolution picture; a rebound pad keeps its port
  across a replug; M3/M4 goldens unchanged.

### D: Published maps and Linux packaging

- `tools/ghidra/publish_maps.py` (contributor tool, `python3 -I`): `work/recomp/maps` → `data/maps/SLUS_21207/*.toml`.
  The core loader reads published maps by default, fills the jump tables' entries from the unit image at `prepare`,
  and checks the source SHA-1; `--maps DIR` keeps the contributor path. Names: kernel and SDK names only, everything
  else generated from the address; first establish whether `ps2_recomp` treats any name specially.
- Checker rules for `data/maps/` (only the published format's keys; no `entries`, no free text).
- **Proof:** the published maps give the same generated C++ as today's maps (identical, or identical apart from
  generated names, each difference explained) and modules that pass the M3/M4 checks on the disc.
- Install layout: `dq8-game`, `dq8-launcher`, `dq8ctl`, `ps2_recomp`, the GS and movie libraries, `data/`,
  `config/`, `cmake/module`, the ABI headers; RPATH `$ORIGIN/../lib`; `share/` lookups next to `bin/`.
- A minimal LGPL FFmpeg build (mpeg2video only; submodule, `cmake/Ffmpeg.cmake`) bundled for `dq8-movie`.
- The AppImage (`packaging/linux/`: launcher, game, tools, libraries, cmake and ninja bundled; the system compiler
  builds the modules) with a desktop entry and icon; a CI job that builds it and scans it with the checker.
- **Owns:** `data/maps/**`, `tools/ghidra/publish_maps.py`, `src/core/recomp/function_map.*` and the `prepare` table
  fill, `scripts/check_no_game_data.py`, install rules in the CMake lists (`install()` lines only), `packaging/linux/**`,
  `third_party/ffmpeg`, `cmake/Ffmpeg.cmake`, `cmake/Movie.cmake`, the CI packaging job,
  `tests/unit/m5_maps_tests.cmake` and its tests, `Docs/formats/function-map.md`.
- **Done when:** a clean checkout builds modules from the disc with no Ghidra and no `work/recomp/maps`; the proof
  above holds; the AppImage runs on a clean Ubuntu 24.04 and passes the scan.

### E: Windows

- `cmake/toolchains/llvm-mingw.cmake`; the whole host (SDL3, paraLLEl-GS and Granite, minicoro fibers, the process
  runner, paths, audio, `dq8-movie` with the LGPL FFmpeg DLLs) cross-built from Linux with a pinned llvm-mingw;
  Windows fixes in the runtime (each in a `#ifdef _WIN32` section E adds); the unit tests under Wine in CI.
- The module build on Windows with the same toolchain (bundled cmake and ninja); the launcher as a GUI-subsystem
  executable.
- The toolchain download: consent, pinned URL and SHA-256, resume, unpack into the user data directory
  (`src/core/toolchain/download.*`, called by A's `detect()` through `DetectOptions::bundled_root`).
- The Windows zip (`packaging/windows/`) and a CI job that cross-builds it, runs the tests under Wine and scans it.
- **Owns:** `cmake/toolchains/**`, `src/core/toolchain/download.*`, the Windows sections it adds, `packaging/windows/**`,
  the CI windows-cross job, `tests/unit/m5_windows_tests.cmake` and its tests. The MSVC job stays a compile check.
- **Done when:** the cross-built host passes the unit tests under Wine and boots the disc to the title under Wine on
  the dev box; the zip passes the scan; the owner's run on real Windows is the integration step I3.

## Merge order and integration

**Merge order:** A → C → D → B → E. Later sessions rebase on what is merged; contract changes go through the
coordinator.

| Step | Settles |
|---|---|
| I1 | The settings round trip: launcher Settings → settings file → `dq8-game` (window mode, 2× picture, aspect auto at 16:9, bindings, volume) |
| I2 | A clean Linux profile: AppImage → wizard → setup (published maps, progress, cancel and resume) → Play → save → restart → Continue; no terminal |
| I3 | Windows under Wine on the dev box (smoke), then the owner on real Windows: zip → wizard → toolchain download → setup → Play |
| I4 | The legal scan of the AppImage and zip; `THIRD_PARTY_NOTICES.md` covers every bundled binary |

## Acceptance

1. Linux and Windows: a new user goes from the ISO to playing without a terminal (wizard, verification, setup with
   progress, cancel and resume, Play), with the published maps and no Ghidra.
2. The settings UI, generated from the schema, persists to TOML and drives `dq8-game`: window modes, vsync, internal
   resolution 1×–4× giving a real higher-resolution picture, aspect including auto 16:9, integer scaling, filter,
   audio device and volume, rumble.
3. Controller mapping: per-device profiles, rebinding, dead zones, hot-plug with stable ports.
4. The diagnostics export holds no game data; errors show stable codes with an action.
5. The M3/M4 goldens are unchanged with default settings; deterministic events are unchanged (status is separate).
6. CI is green on every job including the AppImage and Windows cross builds; checker, pre-commit and REUSE lint are
   clean; release artifacts scan clean.

## Risks

| Risk | Handling |
|---|---|
| The runtime has never run on Windows (fibers, Vulkan through Granite under mingw, audio, paths) | E starts with the cross build and Wine smoke tests; the owner validates on real Windows at I3 |
| Published maps are a legal question (D9) | Shape only: no invented names, no table values; ADR-0017 records it; the checker enforces the format |
| The AppImage against the user's compiler and glibc | Built on an Ubuntu 24.04 base; the toolchain check reports versions; modules use the user's compiler only |
| A 15–40 minute first build | Progress with an estimate from the manifest's timings, cancel and resume, RAM-based jobs, optional ccache |
| A minimal LGPL FFmpeg on both platforms | mpeg2video-only configure; `auto` keeps skipping movies without the library |
| Dear ImGui DPI, fonts and file dialogs | SDL3 content scale, SDL3's native dialogs, a bundled open font |

## Protocol notes

(Sessions append their findings here.)

### Session A: core APIs, settings and the CLI (2026-10-22)

Branch `claude/m5-a-core-d84cfa`. Everything below is in `libdq8core`, `dq8-game` and `dq8ctl`; the launcher (B)
calls the same APIs.

**Contract additions (additive; no day-0 signature changed).**
- `settings.h`: `Store::load` never throws for values: an unknown key is kept and an invalid value dropped (the
  default applies), both as `"<file>:<line>: ..."` lines in `warnings()`, so "never fatal" holds; a file that is not
  TOML is `settings-parse`, and `set()`/`set_from_text()` throw `settings-invalid` (an explicit request). New:
  `use_profile()`/`profile()`, `to_toml()`, `clear()`, `holds()`, `applies()` (the `requires` conditions against the
  effective values), `Schema::categories()`, `StringTable::parse()`/`contains()`, the free functions `to_text()`,
  `parse_value()`, `check_value()`, `describe_allowed()`, `to_string(Type)`, `to_string(Layer)`, and
  `Setting::file_table()`/`file_name()`.
- Where the per-profile layer lives: the user file's `[profile.<id>.<category>]` tables. `Layer::profile` is the
  tables of the profile chosen with `use_profile()` (dq8-game chooses the verified disc's id); saving `Layer::user`
  writes every profile's tables back. A settings file's tables are named by the key's prefix (`display.width` lives
  in `[display]`), which equals `category` for every M5 row. `to_toml(Layer::defaults)` lists every setting
  (`dq8ctl settings defaults`).
- `setup.h`: `Options` gained `ps2_recomp`, `game_executable`, `module_template_dir`, `abi_include_dir`,
  `smoke_input`, `toolchain_root` (passed to `DetectOptions::bundled_root`: session E's hook), `registry_file`,
  `required_free_bytes`, `smoke_timeout`; each empty one is found from `executable_dir` (beside it, then the source
  tree above a build tree, then `PATH` for programs). `Result` gained `work_dir` and `log_file`. Also
  `setup::stages` and `default_work_dir()`. A build tree has `dq8-game` and `ps2_recomp` elsewhere than beside the
  front end, so `dq8ctl` passes the ones its build made (`DQ8_DEFAULT_GAME`, `DQ8_DEFAULT_PS2_RECOMP`); the launcher
  should do the same in its own CMake list.
- `toolchain.h`: `Distribution`, `parse_os_release()`, `this_distribution()`, `install_advice()`,
  `parse_version()`, `compare_versions()`, `cmake_arguments()` (`CMAKE_CXX_COMPILER`, `CMAKE_MAKE_PROGRAM`, so the
  module build uses exactly the detected tools), `DetectOptions::use_environment` (CC/CXX). Minimums: cmake 3.25
  (cmake/module's own `cmake_minimum_required`, not the 3.24 the day-0 comment said), GCC 11, Clang 14 when the
  version is known. The C compiler is reported but not required: cmake/module is `LANGUAGES CXX`.
- New `include/dq8/core/diagnostics.h` (`report()`, `write_report()`; plain C++ types): the report `dq8ctl
  diagnose` prints and the launcher's Diagnostics page exports. The header states the rule: paths, hashes,
  versions, settings and our own logs only; never the contents of a disc file or anything derived from one, no
  RAM, frames, audio or saves, no tool output.
- `paths.h`: `find_data_dir()` and `executable_dir_of()`.
- Shared files touched, approved by the coordinator (2026-10-22): `src/core/CMakeLists.txt`
  (`setup/diagnostics.cpp`), `src/host/CMakeLists.txt` (`settings_wiring.cpp`, `status_channel.cpp` in
  `dq8_host`), new `data/smoke/`, `tests/local/CMakeLists.txt` (the skip run's input path); also approved: the
  `setup::Options` additions and `Store::load`'s warning-and-default while `set()`/`set_from_text()` throw.
- `setup::Options::allow_toolchain_download` (session E's request, approved by the coordinator): the user's
  consent to fetch the bundled toolchain; `dq8ctl setup --allow-download`, the launcher's consent dialog.

**Settings files.** Read as toml11 ordered values and written back from the loaded document: order, comments and
unknown keys (top-level, inside a category table, inside a profile table) survive a save; known keys are replaced
in place, reset keys removed, new ones appended in schema order, emptied tables dropped. A file with a newer
`schema_version` loads with a warning and keeps its unknown keys. Text forms: booleans also take on/off, yes/no,
1/0; a real always prints with a decimal point (`2.0`).

**dq8-game.** `--config`/`--set` go through `settings_wiring.cpp`: the store is loaded before the disc is verified
(a damaged file stops the run at once, exit 2 `settings-parse`), the profile is chosen after it, and a table of
rows (`setting_options()`: key, the flag that wins, the apply function) turns it into `Options`. "Flags win" needs
to know which flags were typed, so `Options::given`/`was_given()` record the command line. A row also skips when
its `requires` conditions do not hold. Session C adds its rows to that table (the marked line) and its fields to
`Options`. Choices made for A's rows: `game.save_dir = ""` leaves `--save-dir` unset, so M4's defaults hold (the
user data directory with `--window`, in-memory cards headless: scripted runs never touch user files);
`audio.output` applies headless too, so a default file asks for the SDL sink there, which sits after the audio hash
(the tests set `SDL_AUDIO_DRIVER=dummy`). With `--json` and a store, the report lists the rows that applied
(`"settings": ["audio.output=null", ...]`); without `--config`/`--set` the report and every event are M4's.

**The status channel.** `started` (sent once the disc is verified, before any settings warning), `first_frame` (the
window's first present; headless, the first vblank), `fps` once a wall-clock second (frames: presents with
`--window`, vblanks headless; `speed`: guest seconds over wall seconds; the first second starts at the first slice,
so start-up is not counted), `warning` (each settings warning, code `settings-invalid`), `exit` (the `end` event's
`ended`, with `requested` reported as `stop`). An exception leaving `run_game` sends `exit` with reason `error` and
the code and status `main()` gives it (`usage`, `module-validation`, `backend`, or the `core::Error` name). A usage
error from option parsing happens before the channel exists: exit 2 and stderr only. `--status -` duplicates stdout
for the channel and points stdout at stderr, so stdout carries status lines only (with `--json` the report moves to
stderr); `--status -` with `--events -` is a usage error. Session C: the present path calls `status.frame()` right
after `platform->present()`; keep it when the window code moves.

**The setup.** `setup::run()` drives the existing pieces: `verify_image` (with the free-space check: 2 GiB less what
the work directory already holds, at least 256 MiB, on the nearest existing ancestor), `extract` into
`<work>/baserom/`, `toolchain::detect`/`check` plus the program's own parts (`ps2_recomp`, `dq8-game`,
cmake/module, the ABI headers: `tool-missing` when one is gone), the maps (contributor `<unit>.csv` in `--maps`,
else published `data/maps/<profile>/<unit>.toml`; `maps-missing` names the units without one), then
`prepare_units`/`generate_units`/`build_units` with `cmake_arguments()`, the smoke test and the registry.
- Progress: bytes for verify and extract, units for prepare and generate (`done` counts the units before the
  current one), ninja's `[n/N]` per unit for build (parsed from the tool output), thinned to whole percents per
  unit; `logs/setup.log` thins further to 5 % and writes an error's continuation lines as `  | `.
- Skipped: extract when every file is up to date; prepare when no ELF or config was rewritten; generate when every
  unit's recorded key matched; build when no module changed (cmake/module relinks every unit on each build, so
  ninja never says "no work to do"; configure and the relink cost about a second a unit); smoke when `stamps/smoke.toml` holds the same manifest and
  `dq8-game` SHA-1s; register when the entry is identical. Verify, toolchain and maps always run (seconds).
- Cancel: the flag is polled between stages and passed to the pipeline and the process runner, which stops the
  whole tool tree; the running stage ends with `setup-cancelled` (`process-cancelled` from the pipeline is mapped).
- The smoke input. The coordinator approved `data/smoke/SLUS_21207.dq8in`, but a serial-named file is refused
  outside `data/regions/` (the checker's forbidden-name rule and `.gitignore`'s `SLUS_*`), so
  `data/smoke/smoke.toml` maps the profile id to `dq8-us-boot-to-title.dq8in`. That script is the one copy: the
  settings-only input moved there from `tests/local/input/boot-to-title-settings-only.dq8in`, and
  `local.boot_to_title_skip` and `test_input_script.cpp` read it from `data/smoke/` (the M3 and M4 bring-up logs
  keep the old name as it was on their dates). Its header says what it does (dismiss the first-run settings
  screen, then no press) and where the smoke uses it. The run is `--headless --movies skip --input-script ...
  --until title --vblanks 3000 --json`, 900 s at most, output in `logs/smoke.log`. A profile without a row runs
  `--until overlay:<its [title] overlay> --vblanks 3000`, one without a `[title]` table 600 vblanks that must end
  without a stop.
- The maps (session D's request through the coordinator). An empty `Options::maps_dir` means the published maps;
  until D merges the stage looks for `data/maps/<profile>/<unit>.toml` itself and passes that directory as
  `PipelineOptions::maps_dir`. D replaces both with `recomp::find_maps_dir(executable_dir)` and
  `PipelineOptions::published_maps_dir` in `stage_maps` and `pipeline_options()` (setup.cpp). Every
  `maps-missing` message names `--maps`.

**dq8ctl.** `setup`, `settings list|get|set|reset|defaults`, `run`, `diagnose` (`src/cli/README.md`). `run` starts
`dq8-game --iso --module-dir --config <user file> --window` for the registered build; options after `--` pass
through (`--headless` replaces `--window`); the game leads its own process group, so Ctrl-C reaches only dq8ctl,
which stops the game through the runner. `diagnose` holds no tool output: compiler messages can quote generated
code, so the setup log's `  | ` lines are left out and the tool logs are listed by name and size only.

**Windows (session E's request through the coordinator).** Wired against E's `src/core/toolchain/download.h` as it
stands in E's worktree, behind `__has_include("toolchain/download.h")` so this branch builds without it (both
paths were syntax-checked against E's header, not built or run):
- `detect()` takes the bundle from `installed_toolchain(bundled_root)` (`<bundled_root>/<version>/`), looks
  there first (`bin/clang++`, `bin/x86_64-w64-mingw32-clang++`), then CC/CXX, then PATH; `Toolchain::bundle_dir`
  is the bundle when the compiler came from it.
- On `_WIN32`, a toolchain stage without the bundle calls `download_toolchain()` with the consent, the bundled
  root, the cmake beside the program (it unpacks with `cmake -E tar`), the cancel flag and a progress callback:
  `toolchain` progress events with `done`/`total` and the phase (`download`, `verify`, `unpack`) as the message,
  thinned to whole percents. Without consent the stage ends with `toolchain-incomplete`, naming the archive's
  size and `--allow-download`. Then it detects again.
- With the bundle the module build gets `-DCMAKE_TOOLCHAIN_FILE=<cmake/toolchains/llvm-mingw.cmake, found like
  the other data>`, `-DLLVM_MINGW_ROOT=<bundle>` and `-DCMAKE_MAKE_PROGRAM=<ninja>` instead of
  `CMAKE_CXX_COMPILER`; a missing toolchain file is `tool-missing` in the toolchain stage.
- `executable_dir_of()` reads `/proc/self/exe` on Linux only (elsewhere `argv0`); E may add `GetModuleFileNameW`.
  `detect()` appends `.exe` to explicit candidate paths; `--status -` uses `_dup`/`_dup2`.

**The build manifest (a fix outside A's list, told to the coordinator).** On the disc a no-op re-run still ran the
smoke test and rewrote the registry: cmake/module relinks every unit on each build, and `build_units` rewrote
`build-manifest.toml` with fresh timings every run, so its SHA-1 (which the smoke stamp and `installs.toml` record)
changed and the real build times were lost. `build_units` now keeps a unit's earlier row when the module comes out
the same from the same inputs (`module`, the module, source, ELF, config, map and generated-tree SHA-1s, the
generator) and writes the manifest only when it changes.

**Results on the dev box (2026-10-22, release preset, the contributor maps).** Measured while other sessions built
in parallel (load average 40 to 50 on 12 threads), so the build time is an upper bound.

| Run | What happened |
|---|---|
| `dq8ctl setup` into a clean user data directory (`XDG_DATA_HOME`, `XDG_CONFIG_HOME` under `work/`) | verify, extract (25 files, 4,110,289 bytes), toolchain (GCC 16.2.1, cmake 4.4.4, ninja 1.13.2), maps, prepare and generate (7 units, 2 s) done; Ctrl-C at build step 316 of 10,505 (main): `build error [setup-cancelled]`, exit 2, no compiler left running |
| The same command again (resume) | extract, prepare, generate `skipped`; the build went on at ninja's step 320 (10,186 steps left) and took 29.7 min with 12 jobs; smoke `done` (the title at the first try, 6 s); register `done` |
| Again, after the manifest fix | 15 s: verify, toolchain, maps `done`; extract, prepare, generate, build, smoke, register `skipped`; the manifest SHA-1 unchanged |
| `dq8-game --headless`, the smoke input, 1,200 vblanks, without and with a file from `dq8ctl settings defaults` | Byte-identical event streams (2,461 events, GS `5db0ec3f…`, RAM `b6a39661…`): default settings reproduce M4 |
| The same with A's rows changed (`audio.output = null`, `latency_ms = 200`, `rumble = false`, `movies = skip`, `save_dir`, `memory_card2 = formatted`) | `movie_skip` ×4 and `movie_timeline_skip` instead of a played movie; `card2_sha1` in the `end` event; with `save_dir` at a copy of M4's B5 folder card the game lists, opens and reads the save on port 0 (`card_sha1 290f8c61…`, B5's); with `audio.output = sdl` the SDL sink runs (its "frames played" line), with `null` it does not. `latency_ms` and `rumble` act only on the SDL sink and the window's pads; their effect on `Options` is covered by `test_host_settings.cpp` and the `--json` list |
| `dq8ctl run -- --headless --input-script <smoke> --until title --speed 1 --status -` with `game.movies = skip` in the user file and `[profile.SLUS_21207.audio] output = "null"` | The title at vblank 1,375; stdout carried only status lines: `started` (profile SLUS_21207), `first_frame` 50 ms, 22 `fps` lines at 59.7 to 60.5 fps and speed 1.00, `exit` status 0 reason `until` |
| `dq8ctl diagnose` on that data directory | System, toolchain, settings with sources, the build and its manifest (as registered), 59 setup-log event lines, the tool logs by name and size; no `  | ` lines, no tool output |

Gates: dev-clang 627/627, asan 627/627, ci-gcc (GCC 16, Release, `-Werror`) 626/626, `check_no_game_data.py --tree
--deep` clean, pre-commit (clang-format) clean, `reuse lint` compliant.

**Left for later.** The published maps (D) were not there yet, so the setup ran with `--maps`; the window paths
(`first_frame` and `fps` from presents, rumble) were checked in code and on the synthetic machine, not with a window
on the disc; the launcher drives none of this yet (B).

### Session C: display, input, audio and widescreen (2026-10-22)

**Coordinator decisions (2026-10-22).** Approved outside C's file list:
1. `WindowConfig` in `src/runtime/platform/platform.h` gains the window mode, filter, integer scaling and an
   `InputConfig` (bindings, dead zone, rumble strength), with defaults equal to M4's behaviour (windowed, linear,
   no integer scaling, no dead zone, full rumble, the M3 bindings), so the null platform, the test fakes and every
   golden stay unchanged.
2. The shared input-profile code lives in core, `include/dq8/core/input_profile.h` and
   `src/core/input/input_profile.cpp`, rather than in a copy in the launcher, since the launcher links only core.
   Conditions: no SDL types or headers in core (sources stay strings, exactly SDL3's spellings); toml11 stays
   private to the .cpp; the pad controls are our own fixed list in the header (`control_names`); unknown or
   invalid entries come back as warnings, not exceptions, as settings files load; tests in
   `m5_display_tests.cmake`; a row in `src/core/README.md`.
3. One-line or one-row additions: `src/game/CMakeLists.txt`, `src/game/README.md`, `data/README.md`,
   `src/host/README.md`, `src/host/CMakeLists.txt` (`display_wiring.cpp`, which keeps C's logic out of
   `run_machine.cpp`). Session A also adds a line to `src/core/CMakeLists.txt` and two to
   `src/host/CMakeLists.txt`; C resolves those one-line conflicts at its rebase onto A.

**dq8-game flags for the C rows** (`src/host/options.*`, one marked block; defaults are M4's behaviour):

| Key | Flag | Notes for the settings step |
|---|---|---|
| `display.window_mode` | `--window-mode windowed\|borderless\|fullscreen` | Alt+Enter toggles between the window and full screen (borderless when the mode is `windowed`); its Return never reaches the pad |
| `display.width`, `display.height` | `--window-size WxH` | One flag for both keys |
| `display.vsync` | `--vsync on\|off` | |
| `display.internal_resolution` | `--internal-resolution 1x..4x` | Refused with `--ssaa` and without `--gs parallel`: the settings step should set it only when the run uses `--gs parallel` and no `--ssaa` was given |
| `display.aspect` | `--aspect auto\|4:3\|16:9\|stretch` | |
| `display.integer_scaling` | `--integer-scaling on\|off` | |
| `display.filter` | `--filter linear\|nearest` | |
| `audio.device` | `--audio-device NAME` | `""`: leave the flag out |
| `audio.volume` | `--volume 0..100` | |
| `input.profile` | `--input-profile NAME\|FILE` | |
| `input.deadzone` | `--deadzone 0..50` | |
| `input.rumble_strength` | `--rumble-strength 0..100` | |

Also new: `--pipeline-cache FILE|none` (default `<user data>/cache/parallel-gs.pipelines` with `--window`, none
headless) and `--dump-window N:PATH` (with `--window`: a PNG of what the window shows at the first frame of vblank N
or later; how the checks below were made). The flag defaults are M4's behaviour, following the goldens rule
(without `--config` or `--set` a run is M4's): `--aspect 4:3` and `--deadzone 0`; the schema's own defaults (`auto`,
8 %) come through `--config`, so the settings step sets every C key from the store, defaults included. With no dead
zone a pad stick passes through as it is; M3's rule that a stick byte between 0x70 and 0x90 does not replace a held
stick key is kept (`sdl_bindings.h`), so a resting stick never cancels a key.

**Widescreen.** The profile's new `[display]` table (`widescreen_variable = 0x003D275C`, `widescreen_width = 1`,
`widescreen_value = 1`; spike S6b's selector byte) feeds `game::DisplayWatcher`, attached only with `--window`; it
reads the byte at every vblank start and emits nothing. On the disc (release build, `--window`): with the first-run
settings at Wide Screen 16:9 (`boot-to-title-settings-only.dq8in`) the game plays `L5_CI_W.PSS` and `TAKA_W.PSS` and
the window shows the logo, the flyover and the title at 16:9 with correct proportions; with Normal 4:3 (Cross, Cross)
the game plays `L5_CI_B.PSS` and `TAKA_N.PSS` (the 4:3 variants are `_B`/`_N` here, not unsuffixed names) and the
window shows 4:3. M4's open "movie geometry" item (our logo 4/3 narrower than PCSX2's) is the 16:9-over-4:3 factor
that `auto` now applies (*inferred*: not re-measured against PCSX2's frame).

**Internal resolution** (`host/display_wiring.h`): paraLLEl-GS's high-resolution scan-out is at most twice the
mode's size (`mode_width << 1` in its renderer) and needs a sampling grid of at least 2 on both axes, so `2x` is 4
samples (2×2), `3x` 8 (2×4) and `4x` 16 (4×4), all with high-resolution scan-out, super-sampled textures and
back-buffer promotion, and all scan out 1024×896 for the 512×448 mode; 3x and 4x only smooth it further. `1x` sets
none of them (M4's picture). The pipeline cache was 24 MB after a title run at 2x. On the disc at `2x` (release,
headless, M4's B2 route under `--movies blank`; frames in the scratchpad, measured as the share of 2×2 blocks with
detail of their own, which is 0 for a pixel-doubled picture): the opening's 3D scene at vblank 2290 has 32.6 % (mean
spread 6.1; edges step in single 2x pixels), so the picture is genuinely higher-resolution; a 2D menu (the
memory-card screen) has 0.6 %, as S5b found for 2D scenes; the same 3D scene during its fade-in (vblank 2050) has
only 1.4 %. *Inferred* from paraLLEl-GS's heuristic (`promote_render_pass_to_backbuffer` accepts a render pass of
textured sprites from one texture only): a fade drawn over the blit disqualifies the promotion, so during fades 2x
falls back to an upscale of the native picture. That answers part of S5b's open question on the heuristic over a
session; a fix would be a paraLLEl-GS patch (`patches/parallel-gs/`), not in M5's scope.

**Determinism.** A headless boot to the title (`boot-to-title-settings-only.dq8in`, `--movies skip`, `--until title`)
gives byte-identical event streams (2,812 lines; title at vblank 1375; gs `ce0e39f3…`, RAM `c8776e04…`, audio
`1efb4699…`) from main's dev-clang binary at `9b004b9`, from this branch, and from this branch with `--aspect stretch
--volume 10 --deadzone 30 --input-profile nosuch --window-mode fullscreen`. 1x and 2x runs of the same script give
the same gs and RAM hashes (5,100 vblanks of the B2 script under `--movies skip`).

**Input.** Bindings come from input profiles (`Docs/formats/input-profile.md`); the parser and writer are SDL-free
in `dq8::core::input` (`include/dq8/core/input_profile.h`) so that the launcher's Controllers page (session B) uses
the same code. Source names are SDL3's own (scancode names; gamepad button and axis strings, `a` = south by
position, `leftx-` for a half axis), so both sides convert with SDL. The built-in profile is M3's table; the switch
tables in `sdl_mapping.cpp` are gone. Ports (`sdl/sdl_ports.h`): by GUID plus serial, a replugged pad gets its port
back; tested with SDL virtual gamepads (unplug both, plug B first: B keeps port 2). A profile may pin a GUID to a port
(`port = 1` or `2`, as players count).

**Audio.** `--audio-device` matches `SDL_GetAudioDeviceName()` exactly; an unknown name falls back to the default
device with one warning and is looked for again at every reopen. `--volume` is `SDL_SetAudioStreamGain` on the
device stream, after `AudioOutput`'s hash and taps. On the disc (SDL's `disk` audio driver, which records what the
device plays; headless boot to the title at `--speed 1`): at `--volume 30` the device got RMS 0.296 and peak 0.300 of
the `--volume 100` run, while the `--audio-dump` WAV files were byte-identical and the audio hash the same.

**Window, on this desktop** (Hyprland, Wayland, 1920×1080; `--dump-window`): `borderless` and `fullscreen` both give
a 1920×1080 window; `--integer-scaling on --filter nearest --aspect 4:3` draws the 512×448 frame at exactly 1195×896
(×2 vertically) centred at (362, 92), every source row doubled. A tiling compositor ignores `--window-size`
(Hyprland tiled the window at 951×1034); floating window managers honour it. vsync is `SDL_SetRenderVSync`
(checked on the renderer in the tests, not measured on the monitor).

**Waits on session A:** the settings step for the C keys (the table above), wired by C at its rebase onto A; a
missing or broken input profile is a warning on stderr today (`window_config()` returns it) and could also become a
`warning` status line. **Not covered:** a real gamepad unplugged and plugged in again (none was connected during the
session; the virtual-gamepad tests cover the logic, and dq8-game reports each pad's port on stderr), `--window-size`
on a floating window manager, and vsync on the monitor.

## Bring-up log

| Date | Step | Result |
|---|---|---|
| 2026-10-22 | Day 0 | Contracts, stubs and the placeholder launcher on `main`; dev-clang and asan unit tests pass (596); the launcher runs three frames on SDL's dummy driver |
| 2026-10-22 | Session C | Window modes and Alt+Enter, aspect auto through the `[display]` watcher (16:9 frames and the `_W` movies at 16:9, 4:3 with Normal 4:3), integer scaling and filter, `--internal-resolution` (a real 2x picture in 3D scenes), pipeline cache, input profiles with dead zones, stable ports and rumble strength, audio device and volume; event streams byte-identical to `main` (see "Protocol notes"); unit tests pass on dev-clang and asan (614) and on GCC with `-Werror` (613); checker `--tree --deep`, pre-commit and `reuse lint` clean |

## Results

(Filled in at the end of M5.)
