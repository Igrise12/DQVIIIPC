# src/core/

Platform-neutral foundation: configuration and settings schema, logging, paths, hashing, disc-image access, game verification, feature registry. No PS2 knowledge beyond file formats, and no UI. Public headers live in `include/dq8/core/`; everything is in namespace `dq8::core`.

What exists (milestone M1, the disc intake):

| Area | Files | Purpose |
|---|---|---|
| errors | `error.cpp`, `error.h` | `Error : std::runtime_error` with a stable `ErrorCode`. Codes are never renamed; `to_string()` gives their kebab-case spelling for `--json`; `is_mismatch()` separates "this is not the disc" from I/O and usage failures. |
| hash | `hash/` | Streaming SHA-1 and CRC-32 (IEEE), `to_hex`. No dependencies. |
| iso | `iso/` | A strict ISO 9660 reader (2048-byte sectors, 64-bit offsets): descriptor scan, directory walk as the source of truth, L and M path tables cross-checked, both-byte-order fields must agree, cycle and depth guards, bounds-checked reads. Interleave, multi-extent and extended attribute records are refused; Joliet, Rock Ridge and UDF are ignored or out of scope (see the header). |
| elf | `elf/` | Minimal ELF32 little-endian header and program-header parser. No section or symbol parsing. |
| overlay | `overlay/` | The `MWo3` overlay-container parser (`mwo3.h`): validates the 64-byte header against the file size and an optional window (magic, id, load address, size identity, constructor-table bounds inside the data range, NUL-padded ASCII name) and exposes the fields, the derived addresses and `image()`, the whole text+data range. `text_size` is a link-map hint, not a code boundary (spike S3). |
| profile | `profile/` | Loads `data/regions/*.toml` with toml11 and validates the schema with `file:line` errors, including the overlay window's loader facts and each overlay row's header integrity values against its segment. |
| intake | `intake/` | `verify_image()` (SYSTEM.CNF, boot-file hash, profile match, ELF cross-check by `segment_index`, then every overlay's container header parsed from the image and checked against its row and segment) and `extract()` (boot file, overlays and listed modules into `baserom/<profile id>/`, 1 MiB streaming, manifest with the overlay headers under `[files.mwo3]`, idempotent). |

Rules:

- Exceptions are fine here (the callers are the CLI and the launcher), but every failure is an `Error` with a code the CLI can map to an exit status.
- Nothing in this directory may name a game, a region or a disc path. Those facts come from `data/regions/`.
- Every parser that sees disc data is bounds-checked and has unit tests on synthetic inputs (`tests/unit/support/iso_builder.h`, `mwo3_builder.h`); fuzz targets are planned (tech-stack.md §8).
- Format constants (`mwo3_header_size`, the magic) live with their parser; addresses and sizes that belong to one disc live in its region profile, never here.
- toml11 is a private dependency of this library; its types never appear in public headers.
