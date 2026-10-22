# Published function map

A published function map tells the setup where one code unit's functions start and end and where its computed-jump
tables are, so a user's setup needs no Ghidra (ADR-0017). It is our own format and holds only addresses, sizes,
counts and table shapes: no instruction bytes, no data values, and no names a tool invented. The jump tables' entries
are read from the user's own unit image while the setup prepares the unit.

Contributors produce published maps from their Ghidra maps (`tools/ghidra/export_maps.sh` writes `work/recomp/maps`,
three files per unit; `include/dq8/core/recomp/function_map.h` describes them) with `tools/ghidra/publish_maps.py`.
The no-game-data checker accepts a file under `data/maps/` only in this format. M5 session D implements the tool,
the reader and the checker rules; until then this page is the day-0 contract (Docs/milestones/M5-launcher.md).

## Where

```
data/maps/<profile>/<unit>.toml      e.g. data/maps/SLUS_21207/menu.toml
```

`<profile>` is the region profile's id and `<unit>` the code unit's name as the recompile pipeline spells it (`main`
for the boot ELF, the overlay's lower-case name otherwise). `dq8ctl recomp` and the setup read these files by default;
`--maps DIR` reads a contributor directory instead.

## Contents

```toml
format = 1
profile = "SLUS_21207"
unit = "menu"
source_sha1 = "<40 hex digits>"     # the unit's file as the region profile records it
code = [0x00461C80, 0x00489A80]     # the unit's code range, end exclusive

# Every function as [start, size]: sorted by start, 4-aligned, inside `code`, no overlaps.
functions = [
    [0x00461D00, 0x40],
    [0x00461D40, 0x1A8],
]

# Return points of bgezal/bltzal/bal calls that leave their function (the contributor map's stats file).
link_return_entries = [0x00462010]

# Kernel and SDK names only; every other function gets a name generated from its address at prepare.
[[named]]
start = 0x00461D00
name = "WaitSema"

# Each computed-jump table: where its words are, the register the jump goes through, and how many entries.
[[jump_table]]
address = 0x00488F00
base_register = 2
count = 12
```

The values above illustrate the shape; they are not taken from the game.

| Key | Rule |
|---|---|
| `format` | 1. A new format number for any change in meaning; readers refuse numbers they do not know |
| `profile`, `unit` | Match the file's place under `data/maps/` |
| `source_sha1` | The SHA-1 the region profile records for the unit's file (the boot ELF, or the overlay's file). A disc whose unit hashes differently is refused with `maps-missing` |
| `code` | The unit's code range; for overlays the profile's overlay window |
| `functions` | `[start, size]` pairs, `size` > 0, sorted, aligned, inside `code`, disjoint |
| `link_return_entries` | Addresses, 4-aligned, inside `code`; may be empty |
| `[[named]]` | `start` is a function's start; `name` is a kernel or SDK identifier (the names our runtime's kernel and stub tables use); optional |
| `[[jump_table]]` | `address` 4-aligned and non-zero; `base_register` 0–31; `count` ≥ 1. The `count` words at `address` are read from the unit image and become the entries 0…`count`−1; each must land inside `code` |

Nothing else is allowed: no `entries`, no comments with text from the game, no other keys. A contributor map whose
table indices are not 0…n−1 cannot be published until the gap is understood.

## How the setup uses it

1. `maps`: find `data/maps/<profile>/<unit>.toml` for every unit of the verified disc and check `source_sha1`.
2. `prepare`: build the recompiler's inputs from it: the functions with their names (named or generated), the
   link-return entries, and each jump table's entries read from the unit image the setup extracted.
3. `generate` and `build` as before. Generated names follow one pattern, so two setups from the same disc and maps
   produce identical code.

Session D proves that published maps give the same generated C++ as the contributor maps (apart from generated
names, each difference explained) and modules that pass the M3 and M4 checks on the disc.
