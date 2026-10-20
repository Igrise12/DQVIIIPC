# data/

Shape-only tables and region profiles (addresses, sizes, expected hashes). Never game bytes. Enforced by the no-game-data check described in Docs/architecture.md §14.

## Region profiles: `regions/<serial>.toml`

One per supported disc, loaded by `dq8::core::load_region_profiles()` and validated with `file:line` errors. `schema_version = 1`:

| Table | Keys |
|---|---|
| `[profile]` | `id` (the name of the directory under `baserom/`; letters, digits, `_`, `-`), `serial`, `title`, `region`, `video_mode` |
| `[image]` | `size` in bytes, `sector_size` (must be 2048) |
| `[boot]` | `path` (disc path, `/`-separated, no `;1`), `size`, `sha1` (40 hex, required), `crc32` (8 hex, required), `entry`, `load_address`, `main_memsz` |
| `[overlay_window]` | `base`, `end`, `container` (the header magic as a name, e.g. `MWo3`), `header_size` (64 for `MWo3`), `code_offset` (where the text begins relative to `base`; must equal `header_size`), and the loader facts from spike S3, each a 4-byte-aligned address inside the main program: `window_pointer` (the data word holding `base`, the loader's destination), `loader`, `loader_hook` (the empty call after the read), `load_by_index`, `name_table`, `lsn_cache`, `ctor_walker`, `flush_cache_stub`, `mode_variable`; `name_table_order`: the overlay names in `name_table` order, one per `[[overlays]]` row, no repeats |
| `[[overlays]]` | `name` (unique), `id` (the header's `overlay_id`: unique, 1 to 64), `index` (the loader's index: the position of `name` in `name_table_order`, unique), `segment_index` (the matching zero-filesize `PT_LOAD` of the boot ELF, counting load segments from 0 = the main image; unique, ≥ 1), `path`, `text_size`, `data_size`, `bss_size`, `table_entries` (what the container header must declare; `header_size + text_size + data_size + bss_size == segment_memsz`, `table_entries * 4 <= data_size`), `segment_memsz` (that segment's memsz), `size` (0 or `header_size + text_size + data_size`), `sha1`, `crc32` |
| `[[files]]` | `path`, `role` (`iop_module`, `iop_image`, `system_cnf`, `other`), `size`, `sha1`, `crc32` |
| `[not_extracted]` | `paths`: disc paths the intake never extracts; a trailing `/` marks a directory |

Sizes of 0 and empty hashes mean "not recorded yet"; `dq8ctl extract --emit-profile` prints the rows with the observed values. The header integrity values are always required: for a new region, read them with `dq8ctl info <file.BIN>` on an overlay extracted by hand (CLAUDE.md shows the 7z command). Verification stops on an unknown boot hash (the region gate, architecture.md §14) and then cross-checks the ELF entry point, load address, main segment size and each overlay's segment (by `segment_index`) and the listed files' presence and sizes; it then parses every overlay's container header from the image and checks `id`, the three sizes and the table entry count against the row and `header_size + text + data + bss` against the segment. The extracted files' hashes are compared on extraction, and `baserom/<id>/manifest.toml` records each overlay's header fields under `[files.mwo3]`.

The file name is the disc serial (architecture.md §13.4), which is why `.gitignore` and the no-game-data check exempt `data/regions/*.toml` from the boot-file-name rules; the content rules still apply.
