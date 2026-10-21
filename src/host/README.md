# src/host/

The game executable (dq8-game). It wires the platform, the runtime, the locally built game modules and the game layer together. Apart from start-up, the run's threads and its reports, it has no logic of its own.

| File | Purpose |
|---|---|
| `main.cpp` | Parses the options, runs, and maps exceptions to exit codes. |
| `options.*` | The command line: `Options`, `parse_options`, the usage text, the exit codes. |
| `session.*` | Start-up shared by every run: disc verification or a developer ELF, module loading and validation, the dispatcher and overlay manager, the set-up and stop parts of the report. |
| `run_machine.cpp` | The M3 run: machine, devices, IOP, game layer, threads, limiter, events, report. The window and the paraLLEl-GS backend are used when the build has them (`DQ8_HAVE_WINDOW` when `dq8_platform_sdl` is linked; `runtime/gs/gs_parallel_backend.h`, which loads `dq8-gs-parallel` from beside the executable). |
| `event_log.*` | The JSON-lines event stream, and the `--trace` echo. |
| `limiter.*` | Wall-clock pacing between slices. |
| `json_writer.h` | The JSON writer. |

## Threads (ADR-0005, ADR-0007)

- **Machine thread.** `Machine::boot`, then `Machine::run` in one-vblank slices. Between slices it handles events, `--until`, the RAM dumps, the end conditions, and the limiter wait.
- **Render thread.** Lives inside `GsPipeline` and drives the GS backend. With `--gs null`, the pipeline runs inline instead.
- **Main thread.** With `--window`, it runs the `IPlatform` loop: events, live input published to the per-vblank latch, and presentation of the newest frame. Otherwise it waits for the machine thread.

Wall time decides only how long the limiter sleeps. It never reaches the guest.

## Command line

```bash
dq8-game --iso <image> --module-dir <dir> [--regions DIR] [options]
dq8-game --elf <file> --module-dir <dir> [--overlay-window BASE:END [--resident ID]] [--disc <image>] [options]
```

| Option | Meaning |
|---|---|
| `--headless` / `--window` | No window (default), or the SDL3 window with live keyboard and gamepad input. M2's `--window BASE:END`, followed by a range, still means the developer overlay window. |
| `--gs null\|trace\|parallel` | GS backend. Default: `null` headless, `parallel` with `--window`. `--gs-trace FILE` writes the trace backend's stream and implies `trace`. `--ssaa N` (1, 2, 4, 8, 16) applies to `parallel`. |
| `--vblanks N` | End the run after N vblanks. |
| `--until COND` | End the run when COND holds: `overlay:NAME`, `mode:N`, `vblank:N`, or `title`. `title` needs the profile's `[title]` marker, which M3 bring-up adds. It is checked at every slice end. |
| `--speed F\|max` | 1.0 is real time. Default: `max` headless, `1.0` with `--window`. |
| `--input-script FILE` | `.dq8in` input (`Docs/formats/dq8-input-script.md`). Its `after` lines see `overlay_resident`, `mode` and `rpc_bind` events. |
| `--record-input FILE` | Write the input the run read as a `.dq8in` file that replays it. |
| `--movies skip\|stop` | Movie player entries return a placeholder at once (default), or stop the guest (`src/game/movies.h`). |
| `--dump-frame N:PATH` | PNG of the scanout of vblank N. Repeatable. |
| `--dump-ram-at N:PATH` | The 32 MiB of RAM after N vblanks. Repeatable. |
| `--events FILE\|-` | JSON-lines events (below). |
| `--trace kernel,sif,rpc,dma,gs` | Turn these trace categories on (`MachineConfig::trace_mask`). Devices emit `trace` events {cat, msg}, which the host prints on stderr. `--trace-events` also writes them into `--events`. |
| `--strict-kernel` | Noop kernel services stop. |
| `--verify-overlays` | Hash each loaded overlay against the profile, and its text at every vblank. |
| `--stop-at-pc ADDR` | Stop when the function entry at ADDR is dispatched. Repeatable. |
| `--stop-at-rpc SID[:FNO]` | Stop when the game binds IOP server SID (`rpc_bind`), or with `:FNO` when it calls function FNO there (`rpc_call`, which turns RPC tracing on). Repeatable. |
| `--disc <image>` | Developer mode: a disc the IOP serves, not verified. Without it the drive is empty. |
| M2: `--stop-at-first-syscall`, `--max-dispatches N`, `--trace-dispatch`, `--trap-mmio`, `--json` | As in M2. With the machine, `--stop-at-first-syscall` goes through `MachineConfig`. |

**Exit status.**
- **0**: the run ended as asked. That means `--until` was met, `--vblanks` was reached (without `--until`), a `--stop-at-pc`, `--stop-at-rpc` or `--stop-at-first-syscall` stop happened, or the window was closed.
- **1**: any other guest stop.
- **2**: usage, I/O, disc verification, or module validation.
- **3**: the GS backend or the window failed to start.
- **4**: `--vblanks` ran out before `--until` held.

**`--json`.** One object at the end of the run:
- the set-up: `mode`, `profile` or `elf_path`, `elf`, `overlay_window`, `resident` and `modules`;
- `machine`: `gs`, `cost_model`, `vblanks`, `cycles`, `gs_hash`, `gs_vsyncs` and `ram_sha1`;
- `ended`: `stop`, `requested`, `until`, `vblanks` or `window-closed`;
- `until`, `overrides` (name, unit, address, kind, calls) and `input_script`;
- `stop`, which keeps M2's members and adds `thread_id` (0 = the scheduler, negative = the interrupt handler fiber); `reason` is `requested` for a `--stop-at-*` stop;
- `dispatches`, `context`, `mmio` and `exit_code`.

A failure prints `{"ok": false, "error": {"code", "message"}, "problems": [...], "exit_code"}`. Its codes are `usage`, `module-validation`, `backend` (exit 3), or a `core::Error` code.

**Events (`--events`).** One JSON object per line: `{"ev", "vblank", "cycles", ...}`. Every field is deterministic, so two runs of the same disc, build and input give identical streams, and so do `--speed 1` and `--speed max`.
- The host emits `start` (mode, entry, cost model, profile, input kind), `gs` after every vblank (stream hash and vsyncs), `ram_dump`, and `end` (ended, gs_hash, ram_sha1, exit_code, reason). The GS backend appears only in the `--json` report, so the `null` and `trace` streams compare equal.
- The game layer emits `overlay_resident`, `overlay_verify`, `mode` and `movie_skip`.
- The devices emit their own events, such as `thread`, `iop_module` and `rpc_bind`.
