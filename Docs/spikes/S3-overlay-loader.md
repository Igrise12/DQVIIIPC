# S3: How does the game load overlays into 0x00461C80?

**Time box:** 2 sessions. **Depends on:** S2 import (`work/ghidra/base`); optionally S7's title save state.
**Answers:** brief Q3, risk R1; drives architecture.md §4.3 and the M1 `MWo3` parser (M1 part 2).

## What is already known (verified from headers; treat as inferred until the loader confirms it)

All six `BIN/*.BIN` files begin with a 64-byte header, little-endian:

| Offset | Size | Field | Observed |
|---|---|---|---|
| 0x00 | 4 | magic | `MWo3` |
| 0x04 | 4 | overlay_id | CASINO 1, VIEWER 2, SHOP 3, MENU 4, BATTLE 5, TITLE 6 |
| 0x08 | 4 | load_address | `0x00461C80` for all six |
| 0x0C | 4 | text_size | CASINO 0x1ADC0, VIEWER 0x240, SHOP 0xF040, MENU 0x195C0, BATTLE 0x23AC0, TITLE 0x4AC0 |
| 0x10 | 4 | data_size | 0x4D80, 0x200, 0x5F80, 0x1500, 0x2680, 0x680 |
| 0x14 | 4 | bss_size | 0x1380, 0x80, 0x2B00, 0x2200, 0x1C80, 0x600 |
| 0x18 | 4 | table_start | absolute address inside the overlay's .data (TITLE `0x466DA0`) |
| 0x1C | 4 | table_end | `(end - start) / 4` = 5, 4, 3, 4, 5, 6 entries |
| 0x20 | 32 | name | lowercase, NUL-padded (`title.bin`, …) |
| 0x40 | … | payload | text, then data; bss is not stored |

Identities that hold for all six: `file_size == 64 + text_size + data_size`, and the boot ELF's six
zero-filesize `PT_LOAD` segments satisfy `memsz == 64 + text + data + bss` in order CASINO (id 1) … TITLE
(id 6). So the whole file, header included, is resident at `0x00461C80` and code begins at `0x00461CC0`.
The ELF holds six `BIN\...` path strings and ten `cdrom0:` strings, and **no** `MWo3` string.

## Questions to answer

1. Which function(s) load an overlay? Arguments (id, name, descriptor pointer)? Is there a descriptor table in
   `.data` (six entries: name pointer, id, maybe LBA and size)?
2. Read path: by file name through `sceOpen`/`sceRead` (fileio RPC `0x80000001`/`0x80000003`) or by sector via
   `sceCdSearchFile` + `sceCdRead` (cdvd RPC ids `0x80000592`, `0x80000595`, …)? Header first then body, or one read?
3. Destination and post-processing: `0x00461C80` with the header resident? bss zeroing? The exact cache-flush
   sequence (`FlushCache` = syscall `0x64`, `iFlushCache` = `0x68`; look for the `FlushCache(0)`/`FlushCache(2)` pair).
4. What are `table_start..table_end`? Hypothesis: a list of function pointers (init/entry/exit) the main
   program calls after loading. Count how many are called and from where.
5. How does control enter the overlay (direct `jal` into the window, a pointer from the table, a global
   "current overlay" variable)? Count `jal`/`j` sites in the main program whose target is inside
   `0x00461C80–0x00489A80`, and calls from each overlay to absolute main-program addresses.
6. Is there self-modifying code? Enumerate stores whose resolved address is inside the window outside the
   loader, and any store into `0x00100000–0x00461C80` text.
7. Where does the game keep "which overlay is resident", if anywhere?

## Method

Work on a copy: `cp -a work/ghidra/base work/ghidra/s3`. Write `tools/ghidra/QueryRefs.java` (committable,
modelled on MGS2's `QueryNative.java`): given address or string seeds, print xrefs, callers, callees and the
containing function to a file under `work/ghidra/s3/`. If S0 found REA usable, the MCP tools (`open_binary`,
`search_bytes`, `xrefs`, `analyze_function`, `batch_decompile`) are the faster way to explore; the Ghidra
project remains the record.

Heuristics in order of expected yield: (1) xrefs to the six `BIN\` strings and the `cdrom0:` strings; (2) code
materialising `0x00461C80` (`lui 0x46` + `addiu/ori 0x1C80`) and loads at `+0xC`…`+0x1C` from a register holding
it; (3) every `syscall` site with `$v1` = 0x64/0x68; (4) a compare against `0x336F574D` (`MWo3` as a
little-endian word), or its absence; (5) SIF RPC bind calls with the ids above. If S7 produced a title-screen
save state, the extension's `PCSX2SaveStateImporter.java` (needs `EmuCore/SavestateZstdCompression=false`) can
confirm what is resident at `0x00461C80` and `+4`.

## Deliverables (findings)

Prose and addresses only: loader function address(es) and signature, read mechanism, destination, flush
sequence, table semantics, entry mechanism, inter-unit call counts, SMC verdict, resident-overlay variable.
Then propose: the region-profile fields for overlays (`overlay_window`, per-overlay `id`, `unit`,
`segment_index`, `memsz`, header layout name), the architecture.md §4.3 edits (primary: hook the loader;
cheaper fallback than fingerprinting: read `overlay_id` at window+4 after each loader call), the hooks the
OverlayManager needs in M2/M3, and whether `table_start..table_end` must be modelled by the dispatcher.

## Must not commit

Decompiler or disassembly output, Ghidra project copies, save states, any bytes. Addresses and counts are fine.
