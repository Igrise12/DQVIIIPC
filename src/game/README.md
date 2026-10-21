# src/game/

Everything that is specific to Dragon Quest VIII: guest function overrides, memory patches, region profiles, symbol tables, overlay metadata. Region and build specifics stay here, never in runtime/.

Library `dq8_game` (`dq8::game`), namespace `dq8::game`, links `dq8::runtime` and `dq8::core`.

What exists (milestone M2):

| Area | Files | Purpose |
|---|---|---|
| units | `unit_specs.*` | Region profile -> `rt::OverlayWindowSpec` (the window) and seven `rt::UnitSpec`s: `main` (table inside `[boot.load_address, + main_memsz)`, entry `boot.entry`, source hash the boot file's) and one per overlay in loader-index order (`title`, `casino`, `viewer`, `battle`, `menu`, `shop` for SLUS-21207: the profile label in lower case), each with its `overlay_id`, its container's hash and a table range inside the window, within its own segment when the profile records the segment's memsz. Unit names are also the module file names (`dq8_unit_<name>`). |

M3 (`Docs/milestones/M3-boots.md`, session E):

| Area | Files | Purpose |
|---|---|---|
| overlay residency | `overlay_residency.*` | ADR-0012. A replace override on `overlay_window.loader_hook` reads the MWo3 header the game's loader just placed in the window. It checks the destination, the id at `window + 4` and the declared sizes against the profile, makes the overlay resident, and emits `overlay_resident`; any mismatch stops. With `--verify-overlays` it also hashes the loaded bytes against the profile and re-hashes the resident text at every vblank. |
| movies | `movies.*` | `[movie]` player entries: `--movies stop` stops with the entry and its argument. `--movies skip` returns `movie_skip_placeholder_result` and emits `movie_skip` with `placeholder=true`. That value is a **placeholder** until bring-up settles the skip return values. |
| IOP | `iop_modules.*` | `[iop]` rows become `rt::IopModuleSpec` in load order, plus the reset image. |
| mode | `mode_watcher.*` | A vblank listener on `overlay_window.mode_variable` that emits `mode` {mode, previous} on change. |
| run conditions | `until.*` | `--until vblank:N|overlay:NAME|mode:N|title`. `title` waits for the profile's `[title]` marker from bring-up. `trigger_from_event` maps machine events to the input-script triggers. |

`overrides/` and `patches/` stay reserved for declarative override and patch data (architecture §4.5, §10.2).

Rules:

- Facts come from `data/regions/<id>.toml`, never from literals in code; a second region is a new profile, not a code change.
- The runtime only ever sees specs. If the runtime needs to ask a game question, it asks through an interface this directory implements.
