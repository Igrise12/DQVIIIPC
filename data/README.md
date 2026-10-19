# data/

Shape-only tables and region profiles (addresses, sizes, expected hashes). Never game bytes. Enforced by the no-game-data check described in Docs/architecture.md §14.

## Region profiles: `regions/<serial>.toml`

One per supported disc, loaded by `dq8::core::load_region_profiles()` and validated with `file:line` errors. `schema_version = 1`:

| Table | Keys |
|---|---|
| `[profile]` | `id` (the name of the directory under `baserom/`; letters, digits, `_`, `-`), `serial`, `title`, `region`, `video_mode` |
| `[image]` | `size` in bytes, `sector_size` (must be 2048) |
| `[boot]` | `path` (disc path, `/`-separated, no `;1`), `size`, `sha1` (40 hex, required), `crc32` (8 hex, required), `entry`, `load_address`, `main_memsz` |
| `[overlay_window]` | `base`, `end`, `container` (the header magic as a name, e.g. `MWo3`), `header_size` |
| `[[overlays]]` | `name`, `id` (unique, ≥ 1), `path`, `segment_memsz` (the matching zero-filesize `PT_LOAD` segment's memsz, in segment order), `size`, `sha1`, `crc32` |
| `[[files]]` | `path`, `role` (`iop_module`, `iop_image`, `system_cnf`, `other`), `size`, `sha1`, `crc32` |
| `[not_extracted]` | `paths`: disc paths the intake never extracts; a trailing `/` marks a directory |

Sizes of 0 and empty hashes mean "not recorded yet"; `dq8ctl extract --emit-profile` prints the rows with the observed values. Verification stops on an unknown boot hash (the region gate, architecture.md §14) and then cross-checks the ELF entry point, load address, main segment size and the overlay segment sizes, and the listed files' presence and sizes; the extracted files' hashes are compared on extraction.

The file name is the disc serial (architecture.md §13.4), which is why `.gitignore` and the no-game-data check exempt `data/regions/*.toml` from the boot-file-name rules; the content rules still apply.
