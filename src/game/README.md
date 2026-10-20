# src/game/

Everything that is specific to Dragon Quest VIII: guest function overrides, memory patches, region profiles, symbol tables, overlay metadata. Region and build specifics stay here, never in runtime/.

Library `dq8_game` (`dq8::game`), namespace `dq8::game`, links `dq8::runtime` and `dq8::core`.

What exists (milestone M2):

| Area | Files | Purpose |
|---|---|---|
| units | `unit_specs.*` | Region profile -> `rt::OverlayWindowSpec` (the window) and seven `rt::UnitSpec`s: `main` (table inside `[boot.load_address, + main_memsz)`, entry `boot.entry`, source hash the boot file's) and one per overlay in loader-index order (`title`, `casino`, `viewer`, `battle`, `menu`, `shop` for SLUS-21207: the profile label in lower case), each with its `overlay_id`, its container's hash and a table range inside the window, within its own segment when the profile records the segment's memsz. Unit names are also the module file names (`dq8_unit_<name>`). |

`overrides/` and `patches/` are reserved for M3 onwards (architecture §4.5, §10.2).

Rules:

- Facts come from `data/regions/<id>.toml`, never from literals in code; a second region is a new profile, not a code change.
- The runtime only ever sees specs. If the runtime needs to ask a game question, it asks through an interface this directory implements.
