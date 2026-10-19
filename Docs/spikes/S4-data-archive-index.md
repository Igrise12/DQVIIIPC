# S4: The DATA.HD6 / DATA.DAT archive layout

**Time box:** 1 session. **Depends on:** S2 import (for the reader xrefs). **Answers:** brief Q4, risk R6.
Not an M1 gate: the game reads its archives itself through sector I/O; we need the layout for tooling and for
the runtime's disc latency model, not for booting.

## What is already known

The disc holds `DATA.DAT` (1,804,218,368 B) and `DATA2.DAT` (752,971,776 B) **and** two small companions,
`DATA.HD6` (188,732 B) and `DATA2.HD6` (87,092 B), plus `DMYDATA` (51,166,209 B, presumably padding). The boot
ELF contains exactly one `.HD6` string and no `DATA.DAT` string, so the `.HD6` files are the indexes. `DATA.HD6`
begins with the magic `HD6\0`.

## Steps

1. **Scratch parser** (session scratchpad, `python3 -I`, reading `baserom/raw/DATA.HD6` read-only): decode the
   header (first hypothesis: 0x34 bytes; a word equal to the file size; a string table starting at 0x34;
   entry tables with a 16-byte stride). Determine: header fields, string-table bounds, entry count(s), entry
   layout. Validate the hypothesis "each entry carries (sector offset, size) into `DATA.DAT`" by checking that
   offsets are ascending, 2048-aligned, and that the sizes sum to about 1,804,218,368. Repeat for `DATA2.HD6`
   against `DATA2.DAT`. Spot-check two entries by streaming the corresponding sectors from the ISO
   (`7z e -so "$DQ8_ISO" DATA.DAT | dd ...` into `/dev/null` with a hash) and comparing a recognisable
   structure (a magic tag, not content).
2. **Reader in the ELF** (copy `work/ghidra/base` to `work/ghidra/s4`, or REA if S0 passed): xrefs from the
   `.HD6` string → the function that loads the index; from there, how entries are resolved (by name hash? by
   index?) and whether reads go through `sceCdRead` with an LBA base from `sceCdSearchFile`. Where does the
   in-memory index live and how large is it?
3. **Extension histogram** of the string table (count per extension, e.g. `.chr` ×N), and what `DATA2.DAT` and
   `DMYDATA` are for.

## Deliverables

- `Docs/formats/hd6.md`: field tables for the header, string table and entry records; the lookup mechanism;
  the reader's address. Counts and extension histogram only, **no file names** from the table.
- `tools/extract/hd6.hexpat`: an ImHex pattern we author for the format (layout, no values).
- Findings: brief §6 row for the archives, tech-stack §7 (ImHex), and whether M1's `not_extracted` policy
  holds (it should: nothing here is needed to boot).

## Must not commit

Names from the string table beyond counts and extensions, any extracted entry, hexdumps.
