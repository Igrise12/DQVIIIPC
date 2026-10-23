# Published function map

A published function map tells the setup where one code unit's functions start and end and where its computed-jump
tables are, so a user's setup needs no Ghidra (ADR-0017). It is our own format and holds only addresses, sizes,
counts and table shapes: no instruction bytes, no data values, and no names a tool invented. The jump tables' entries
are read from the user's own unit image while the setup prepares the unit.

Contributors produce published maps from their Ghidra maps (`tools/ghidra/export_maps.sh` writes `work/recomp/maps`,
three files per unit; `include/dq8/core/recomp/function_map.h` describes them) with `tools/ghidra/publish_maps.py`.
The no-game-data checker accepts a file under `data/maps/` (and `share/dq8/maps/` in a release) only in this format.
The reader is `parse_published_map()` and `make_function_map()` in `include/dq8/core/recomp/function_map.h`.

## Where

```
data/maps/<profile>/<unit>.toml      e.g. data/maps/SLUS_21207/menu.toml
share/dq8/maps/<profile>/<unit>.toml in an install (bin/../share/dq8/maps)
```

`<profile>` is the region profile's id and `<unit>` the code unit's name as the recompile pipeline spells it (`main`
for the boot ELF, the overlay's lower-case name otherwise). `dq8ctl recomp` and the setup read these files by default
(`find_maps_dir()`: `data/maps` in the working directory or above the executable, else `share/dq8/maps`); `--maps
DIR` reads a contributor directory instead.

## Contents

```toml
format = 1
profile = "SLUS_21207"
unit = "menu"
source_sha1 = "<40 hex digits>"
code = [0x00461C80, 0x00489A80]

functions = [
    [0x00461D00, 0x40],
    [0x00461D40, 0x1A8],
]

link_return_entries = [
    0x00462010,
]

[[named]]
start = 0x00461D00
name = "WaitSema"

[[jump_table]]
address = 0x00488F00
base_register = 2
count = 12
```

The values above illustrate the shape; they are not taken from the game.

| Key | Rule |
|---|---|
| `format` | 1. A new format number for any change in meaning; readers refuse numbers they do not know |
| `profile`, `unit` | Match the file's place under `maps/` (`[A-Z0-9_]+`, `[a-z0-9_]+`) |
| `source_sha1` | Lower case. The SHA-1 the region profile records for the unit's file (the boot ELF, or the overlay's file). A disc whose unit hashes differently is refused with `maps-missing` |
| `code` | The unit's code range `[start, end)` as the pipeline computes it from the disc (`CodeUnit`): main's executable load segment; for an overlay its file's range, from the load address over the header, text and data. Prepare refuses a map whose range differs (`map-invalid`) |
| `functions` | `[start, size]` pairs: `size` > 0, both 4-aligned, sorted by start, disjoint, inside `code` |
| `link_return_entries` | Addresses: 4-aligned, sorted, unique, inside `code`; may be empty |
| `[[named]]` | Optional, sorted by `start`. `start` is a function's start; `name` is a name in the runtime's call list (`include/dq8/abi/ps2recomp/ps2_call_list.h`: its kernel calls and library stubs), matched as the recompiler matches it: exactly, or with one leading underscore removed or added (the map keeps the SDK's spelling, e.g. `_EnableIntc`) |
| `[[jump_table]]` | Sorted by `address`, each address once: non-zero and 4-aligned; `base_register` 0–31; `count` ≥ 1. The `count` little-endian words at `address` are read from the unit ELF and become the entries 0…`count`−1; each must land inside `code` |

Every number is a TOML integer (the tool writes addresses as eight hex digits, sizes in hex). Nothing else is allowed:
no `entries`, no other keys, no strings but the four above, and no comments at all (`data/maps/README.md` explains
the directory). A contributor map whose table indices are not 0…n−1 cannot be published until the gap is understood.

## Names

Every function without a `[[named]]` row gets the generated name `FUN_` followed by its start in eight lower-case hex
digits (`FUN_00461d00`), the placeholder Ghidra gives an unnamed function, so code generated from a published map
and from a contributor's map uses the same names wherever the contributor map had a placeholder.

`[[named]]` keeps only names the recompiler acts on. `ps2_recomp` (third_party/PS2Recomp at `eb8688b`) reads names
from the map in these places, established for M5 session D (2026-10-22):

| Where | What a name does | Published map |
|---|---|---|
| `isStubFunction()` → `ps2_runtime_calls::isStubName()` | A function whose name resolves to a library stub is emitted as a call into `ps2_stubs::` even without a binding | Kept: stub names are call-list names |
| Generated stubs (`resolveSyscallName`, `resolveStubName`) | A bound or stubbed function dispatches to `ps2_syscalls::<name>` or `ps2_stubs::<name>` (our config binds every call-list name by address, `stub_bindings()`) | Kept: kernel-call names are call-list names |
| `makeName()` | The C++ identifier is the sanitised name plus `_0x<start>`; per-function file names follow it | Generated names keep the identifiers of placeholder functions; other names change identifiers only |
| `isEntryFunctionName()` | Names starting `entry_` are the recompiler's own split-off entry functions | Never published: neither a call-list name nor the generated pattern |
| `IsCorrectnessCriticalFunctionName()` | C++ static-initialiser prefixes (`__sinit_`, `_GLOBAL__sub_I_`, ...) | None in the stripped ELF's maps |
| `IsAutoGeneratedName()` (`sub_`, `FUN_`, `LAB_`, `DAT_`) | Only breaks ties between symbols and map rows at one address; the units have no symbols | No effect |
| `[general].skip` and `stubs` selectors by name | Our config uses address selectors only | No effect |

So names outside the call list (Ghidra's `entry`, thunk names that carry their target, kernel labels the runtime
does not implement) change only C++ identifiers, never what the code does.

## How the setup uses it

1. `maps`: `require_published_maps()` finds `<profile>/<unit>.toml` for every unit of the verified disc and checks
   `source_sha1` against the region profile and `code` against the unit.
2. `prepare`: `prepare_units()` with `published_maps_dir` checks the map against the unit file it read, builds the
   recompiler's inputs with `make_function_map()` (names, link-return entries, each jump table's entries read from
   the unit ELF), writes `units/<unit>.csv` (the recompiler's function list) and the config as before.
3. `generate` and `build` as before. Two setups from the same disc and maps produce identical code.

`python3 -I tools/ghidra/publish_maps.py --check` fails when `data/maps` differs from what the contributor maps
would publish. The proof that published maps give the same generated C++ as the contributor maps, and modules that
pass the M3 and M4 checks on the disc, is in Docs/milestones/M5-launcher.md (Protocol notes, session D).
