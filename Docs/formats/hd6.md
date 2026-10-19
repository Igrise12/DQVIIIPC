# HD6 archive index

| | |
|---|---|
| **Status** | Layout, units and the game's reader established by spike S4 (2026-10-19). Member formats are not covered. |
| **Source** | `Docs/spikes/S4-data-archive-index.md` (Findings), evidence under `work/hd6/` (git-ignored); the reader's decompilation lives under `work/hd6/ghidra/` and is never copied here |
| **Rule** | This page describes layout only. Dictionary strings, path records and member data are game-derived and never appear in the repository or in tool output (architecture.md §14). Counts, offsets, sizes, extensions and 4-byte magic tags are allowed. |
| **Pattern** | `tools/extract/hd6.hexpat` (ImHex) encodes the layout below |

## Files

Two archives, each a flat `.DAT` body plus a small `.HD6` index beside it in the disc root. The `.HD6` is the only
thing the game parses; the body is read by sector.

| File | Size (bytes) | ISO LBA | Members | SHA-1 |
|---|---|---|---|---|
| `DATA.HD6` | 188,732 | 2,162 | 9,954 files + 1 terminator | `5b02cdb216a354328ce2232be7df7134e9109a10` |
| `DATA.DAT` | 1,804,218,368 | 1,135,132 | | `0edf8dd0b478805e04fe2516dadcdfa07653c252` |
| `DATA2.HD6` | 87,092 | 2,119 | 4,139 files + 1 terminator | `95adef78ce9cff028ceb2ccbc8b76f7200b0aa89` |
| `DATA2.DAT` | 752,971,776 | 760,313 | | `fe0bcd0cbf2b1f522d498be296c4efec30d68252` |
| `DMYDATA` | 51,166,209 | 2,016,098 | not an archive, see below | `29a18218de919a652bc3a566a239095b3e3c62a4` |

The volume has 2,041,088 sectors; `DATA.DAT` ends at sector 2,016,097 and `DMYDATA` runs from there to 2,041,081,
so the two archives and the filler occupy the outer 63 % of the disc.

## Layout

A 52-byte header, then three sections in this order, each starting at the offset the header gives:

```
Header (0x34)  |  Dictionary  |  Path records  |  Entries (8 bytes each)
```

### Header (52 bytes, little-endian u32)

| Offset | Field | `DATA.HD6` | `DATA2.HD6` | Read by the game |
|---|---|---|---|---|
| 0x00 | magic `HD6\0` | | | no: the magic is never checked |
| 0x04 | dictionary offset | 0x34 | 0x34 | loader, encoder |
| 0x08 | dictionary size (bytes) | 32,828 | 37,064 | loader (bound of the separator rewrite) |
| 0x0C | dictionary string count | 4,558 | 4,170 | encoder (bound of the linear search) |
| 0x10 | 0 | 0 | 0 | no |
| 0x14 | path-record table offset | 0x8070 | 0x90FC | loader, lookup |
| 0x18 | path-record table size (bytes) | 76,212 | 16,856 | no |
| 0x1C | 0 | 0 | 0 | no |
| 0x20 | size unit (bytes) | 16 | 16 | lookup (multiplies the size field) |
| 0x24 | entry count | 9,955 | 4,140 | loader (sort), lookup (search) |
| 0x28 | entry table offset | 0x1AA24 | 0xD2D4 | loader, lookup |
| 0x2C | 0 | 0 | 0 | no |
| 0x30 | file size | 188,732 | 87,092 | no: the loader sizes the buffer with `lseek` |

Both the dictionary and the path table are padded with NULs to a 4-byte boundary; the entry table runs to the end
of the file (`entry count × 8` bytes).

### Dictionary

NUL-terminated byte strings, indexed from 0 in order of appearance. Index 0 is the empty string and the last index
is an empty string too (it is what the terminating entry's record points at). The strings are **path fragments**:
the leading name of a path, then fragments that each begin with the separator that introduced them: `\` on disc
(a directory or file boundary; the loader rewrites every `\` in the dictionary to `/` after reading it), `.`
(extension), `_` or `-` (name parts). `DATA.HD6` holds 4,556 real fragments: 30 start with `.`, 2,385 with `\`,
2,141 are leading names or `_`/`-` parts; lengths 2 to 14 bytes. `DATA2.HD6` holds 4,168 (2 extensions, 55 with
`\`, 4,111 others).

The dictionary is **sorted by descending use count** (verified for both files), so the 127 most-used fragments get
one-byte codes in the records.

### Path records

One NUL-terminated record per entry, in entry order, stored back to back. A record is a sequence of dictionary
indices in a variable-length code:

| First byte | Encoding |
|---|---|
| `b < 0x80` | one byte, index `b` |
| `b >= 0x80`, then `n` | two bytes, index `(b & 0x7F) | (n << 7)` |

The path is the concatenation of the indexed fragments; there are no separators outside the fragments. Records
have 1 to 10 codes and 2 to 16 bytes plus the NUL in `DATA.HD6` (1 to 8 codes, up to 13 bytes in `DATA2.HD6`).
The game encodes a requested path with exactly this scheme (encoder at `0x00164a60`) and compares records
byte-wise, so lookups never decode a name. Decoded, all 9,954 real paths of `DATA.HD6` are 6 to 49 characters
with 1 to 4 directory levels (3 files sit at the root) under 19 top-level directories; the 4,139 of `DATA2.HD6`
are 5 to 38 characters, mostly flat.

### Entries

8 bytes each; a little-endian 64-bit bitfield, least significant field first, unpacked exactly this way by the
game's lookup (`0x00164570`):

| Bits | Width | Field | Unit |
|---|---|---|---|
| 0–17 | 18 | path record offset, from the start of the path table | bytes |
| 18–39 | 22 | data offset inside the `.DAT` | 2,048-byte sectors |
| 40–63 | 24 | data size | header size unit (16 bytes) |

Limits implied: 256 KiB of path records, 8 GiB archives, 256 MiB members. Observed maxima: record offset 76,206
and 16,853; sector 880,740 and 367,647; size 5,762,768 bytes (`DATA`) and 21,124,336 bytes (`DATA2`).

On disc the entries are in **archive order**: sector offsets ascend with the entry index, members are packed back
to back, every member starts on a sector boundary and the padding up to the next boundary is zero-filled (checked
for every member of both archives against the image). The sum of the sector-rounded sizes equals the `.DAT` size
exactly, so there is no slack and no hidden data. `DATA.HD6` has 4 zero-size members (`.cfg`, `.chr`, `.nav`),
each sharing the sector of the member after it, and one path that occurs twice with the same size; `DATA2.HD6`
has one duplicated path.

The **last entry is a terminator**: its record is a single two-byte code for the final empty dictionary string
(so the path is empty), sector 0 and size 0. After the game's sort it is the first entry and is never matched.

## How the game uses it

All addresses are in the NTSC-U boot ELF (`SLUS_212.07`) as analysed by spike S2; names are ours.

**Start-up.** `archive_init` (`0x00164520`, called from the start-up function `0x00160b00`) calls
`archive_open` (`0x00164710`) twice, for the base names `DATA` and `DATA2`, filling a two-element table of
8-byte archive objects at `0x003e9180` (`{u32 lsn; void* index}`, constructed by the static initialiser
`0x003c88e0`) and setting the archive count at `0x003d26dc` to 2.

**`archive_open`** builds `<base>.HD6;1` and `<base>.DAT;1` paths from two stored 256-byte prefixes
(`0x00394e30`, `0x00394f30`), then:

1. opens the `.HD6` through the EE file-I/O RPC (`sceOpen`-style wrapper at `0x0011bba8`), seeks to the end to
   learn its size, allocates `ceil(size / 2,048) × 2,048` bytes (16-byte aligned; `DATA.HD6` therefore takes
   190,464 bytes of EE RAM and `DATA2.HD6` 88,064), reads the whole file into it and closes it. A debug message
   reports the size and warns above 100 KiB (0x19000), which `DATA.HD6` exceeds;
2. rewrites every `\` in the dictionary to `/`;
3. stores the path-table pointer in `0x003d26d8` and `qsort`s the entry table in place (`0x001339f0`, element
   size 8) with the comparator `0x001649d0`, which compares the two entries' records byte-wise (`0x00164a00`,
   a `strcmp` over the encoded bytes). The in-memory table is therefore ordered by encoded path, not by sector;
4. calls `sceCdSearchFile` (wrapper `0x0012a970`, RPC client bound at `0x0012a670` to server `0x80000597`) on
   the `.DAT` path until it succeeds, then `sceCdSync(0)` (`0x0012ab80`) and `sceCdGetError` (`0x0012b928`)
   until the error is 0, and keeps the file's **LSN** in the archive object. The `.DAT` size is never read.

**Lookup** (`0x00164570`, `archive_find(archive, out[5], path)`): encodes the path with the archive's dictionary
(`0x00164a60`: split at `/`, `.`, `_`, `-`; each fragment looked up by a linear `strcmp` scan over the dictionary;
an unknown fragment fails the lookup), then binary-searches the sorted entry table with the byte-wise record
compare. On a hit it returns `{record offset, lsn + sector, size units × size unit, 0, 0}`, so the **absolute
disc LSN** of the member and its size in bytes.

**File handles.** A 12-slot table of 124-byte (0x7C) stream objects at `0x003ee2c0` serves all reads. `open`
(`0x00161ed0`) takes a type and a path with a `<drive>:` prefix that it skips. Type 1 is an archive member: it
tries `archive_find` on each archive in order (`DATA` first, then `DATA2`), and fills the handle with LSN, byte
size, sector count `ceil(size / 2,048)` and a sector size of 2,048. Type 4 is a plain ISO file resolved with
`sceCdSearchFile` (used for overlays and movies, which are not in the archives). Type 0 is a file-I/O RPC file
and type 2 a host-file path whose read stub (`0x001662b0`) returns −1 in this build.

**Read** (`0x001611a0` → `0x00161880`): splits requests into chunks of at most 1 MiB, converts the handle's
byte position to a sector index and the length to a sector count (clipped to the member's sector count), and
issues `sceCdRead(lsn + index, count, buffer, mode)` (`0x0012b5b8`, N-command server `0x80000595`, function 1;
try count 0, spindle control from a handle flag, data pattern 0), then `sceCdSync(0)` and `sceCdGetError`,
retrying after a 100 ms delay while the error is non-zero (a tray-open error re-runs the disc check at
`0x001610b0`). Reads are whole sectors into the caller's buffer, so a member's last partial sector is read in
full.

**No transformation.** Bytes go from `sceCdRead` into the caller's buffer unchanged; nothing on this path XORs,
decompresses or checks the data. The movie table found by spike S6a is not involved: the `.MVI` files are not
archive members (no such extension in the index) and are opened as ISO files by the movie code
(`0x0019f2f0` builds `cdrom0:\MOVIE\` paths).

## Members by extension

`DATA.HD6` (9,954 files, 1,793,836,320 bytes of payload):

| Extension | Files | Bytes | Largest | Smallest | First 4 bytes |
|---|---|---|---|---|---|
| `.chr` | 4,454 | 732,514,816 | 3,089,072 | 0 | varies (457 distinct) |
| `.snd` | 1,145 | 146,354,496 | 1,054,688 | 816 | varies |
| `.pak` | 696 | 204,982,592 | 1,949,168 | 4,128 | varies |
| `.stb` | 589 | 99,037,008 | 452,496 | 1,024 | binary, one pattern |
| `.cfg` | 554 | 1,901,312 | 69,984 | 0 | text |
| `.bin` | 515 | 5,821,824 | 206,000 | 16 | binary |
| `.txt` | 402 | 1,924,064 | 164,672 | 48 | text |
| `.map` | 338 | 6,978,912 | 120,368 | 912 | `IMG ` (321), `PCP ` (16) |
| `.mpk` | 334 | 146,758,656 | 5,762,768 | 4,736 | varies |
| `.ipk` | 321 | 265,119,984 | 2,561,648 | 304 | varies |
| `.img` | 180 | 53,607,296 | 1,581,712 | 5,264 | binary, one pattern |
| `.sky` | 112 | 31,107,536 | 322,800 | 26,608 | `sky2` (107), `sky0` (5) |
| `.pac` | 110 | 43,876,720 | 1,660,272 | 2,448 | varies |
| `.mds` | 90 | 646,192 | 230,656 | 896 | ` MDS` |
| `.nav` | 78 | 49,335,584 | 2,292,816 | 0 | varies |
| `.str` | 7 | 102,688 | 37,792 | 1,696 | |
| `.lst` | 7 | 170,976 | 41,952 | 496 | |
| `.mes` | 6 | 128,976 | 45,744 | 9,872 | binary |
| `.lgt` | 4 | 163,408 | 53,200 | 3,856 | `MPL_` |
| `.efp` | 3 | 48,560 | 25,584 | 11,472 | |
| `.pcl` | 2 | 1,984 | 1,584 | 400 | `PCL_` |
| `.tm2`, `.tm2pak` | 1 each | 131,200 / 1,572,864 | | | `TIM2` |
| `.ico`, `.mot`, `.pac_`, `.bat`, `.cmp` | 1 each | 49,728 / 48 / 999,920 / 48 / 498,928 | | | ` MOT` for `.mot` |

`DATA2.HD6` (4,139 files, 748,833,600 bytes): 4,138 `.mwv` (727,709,264 bytes, 4,480 to 20,386,480 each) and
one `.wav` (21,124,336). All 4,139 are `RIFF`/`WAVE` files: 4,072 mono 32 kHz and 66 stereo 48 kHz with format tag
1365 (0x0555, block align 18 and 36; not a Microsoft tag, presumably Sony's PS2 ADPCM, to be confirmed when the
sound stack is analysed), and the `.wav` is PCM (tag 1) stereo 48 kHz. Besides `fmt ` and `data`, 4,138 of them
carry `pflt`, `fltr`, `ctrl` and `algn` chunks and 65 a `cmnt` chunk. `DATA2.DAT` is therefore the voice/stream
audio archive.

`DMYDATA` is a 51 MB text file of 1,054,720 lines of which only 4 are distinct: filler that pads the volume to
the full 4,180,148,224-byte DVD. The ELF never references it.

## Consequences for the port

- **Runtime disc model (architecture.md §8.3):** the archives are read with `sceCdRead` at absolute LSNs, in
  whole sectors, at most 512 sectors per call, after a one-time `sceCdSearchFile` per archive. The HLE needs the
  ISO directory (for `sceCdSearchFile`) and random sector access into the image; it never needs the index.
  Latency modelling can key on (LSN, sector count) alone.
- **M1 `not_extracted` policy holds:** nothing in either archive is needed to build or boot the game module. The
  two `.HD6` files are small and may be extracted for tooling, but the runtime does not need them either.
- **Extraction tooling** (`dq8ctl`, later): list and extract members from the user's disc by walking the entries
  and reading `lba(DATA.DAT) + sector`; names are decoded from the dictionary for the user's machine only.
- **Game-derived content:** the dictionary, the path records and every member are game data (architecture.md
  §14). Tool output may print counts, extensions and sizes; never names.
