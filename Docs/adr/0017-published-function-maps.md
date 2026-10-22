# ADR-0017: Function maps are published as address-only data; jump-table entries come from the user's disc

| | |
|---|---|
| **Status** | **Accepted** 2026-10-22 by the project owner (M5 planning; decides brief §14 D9) |
| **Related** | architecture.md §3, §14; brief §9, §14; ADR-0002, ADR-0008; tools/ghidra/export_maps.sh; Docs/milestones/M5-launcher.md |

## Context

The setup pipeline recompiles each code unit from a function map: where each function starts and ends, plus the
computed-jump tables. Until M5 the maps are produced by Ghidra on a contributor's machine (`tools/ghidra/export_maps.sh`)
and stay local (D9's proposal), so no end user can run the setup. Architecture §3 always intended that end users never
run Ghidra. The maps were also refined by hand during bring-up (M4 B2: MENU grew from 263 to 418 functions), so
regenerating them on a user's machine would not reproduce them anyway.

## Decision

- The repository carries **published maps** in `data/maps/<profile>/<unit>.toml`, keyed by the unit's source SHA-1
  from the region profile, holding only:
  - each function as a start address and a size;
  - each jump table as its address, base register and entry count.
- They hold **no invented names** (functions get neutral names generated at setup from the address; kernel and SDK
  identifiers, which brief §9 allows, may stay where the recompiler needs them) and **no values read from the game**
  (the jump tables' entries are read from the user's disc during `prepare`). The format is
  `Docs/formats/function-map.md`.
- A contributor tool (`tools/ghidra/publish_maps.py`, run with `python3 -I`) turns the contributor maps into this
  format; the no-game-data checker enforces the format for `data/maps/`.
- `dq8ctl recomp` and the setup API use the published maps by default; `--maps DIR` keeps the contributor path.

## Consequences

- A new user's setup needs only their disc, a compiler and the program: no Ghidra.
- What is committed is addresses, sizes and table shapes, which the project's rules allow; a published map refuses a
  disc whose unit hash differs.
- Map improvements found in bring-up reach users through the repository.
- The legal review in brief §9 still covers these files before 1.0.
