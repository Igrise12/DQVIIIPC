# data/maps

Published function maps (ADR-0017): one file per code unit of a region profile, `<profile>/<unit>.toml`, in the
format of [Docs/formats/function-map.md](../../Docs/formats/function-map.md). They hold addresses, sizes, counts and
the kernel and SDK names of the runtime's call list; the jump tables' entries are read from the user's disc during
the setup. `dq8ctl recomp` and the setup read them by default; an install carries them in `share/dq8/maps`.

Regenerate them from contributor maps with `python3 -I tools/ghidra/publish_maps.py` (and check them with
`--check`). The no-game-data checker refuses any other content here: no table entries, no comments, no other names.
