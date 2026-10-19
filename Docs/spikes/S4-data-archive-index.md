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

## Update from spike S2 (2026-10-19)

- Copy `work/ghidra/base` (it has the `bss` and `overlay_window` blocks); analysis of the main ELF is already done,
  so your runs are `-process SLUS_212.07 -noanalysis -readOnly -postScript <script>` and take 15–30 s each.
  `tools/ghidra/headless.sh <project> DQ8 ...` sources `tools/local.env` and puts `tools/ghidra` on the script path.
- The census found 97 references to I/O registers, 29 to the scratchpad and 13 to IOP RAM in the main program; the
  CD read path you are looking for is among the callers of the `sceCd*` stubs (51 kernel-call stubs carry SDK names
  from the extension's patterns; SIF RPC binds are the way to the IOP).
- The S6a spike found that the movies are XORed with a 2,048-byte table generated at run time; watch for the same
  generator or table on the `DATA.DAT` read path.

## Findings

**Session:** 2026-10-19, one session (about two hours; seven read-only headless Ghidra runs of 20–40 s each under
`nice -n 19 -max-cpu 2`, the rest single-threaded Python). **Stop reason:** done.

Evidence lives under `work/hd6/` (git-ignored): `probe1-5-*.txt` (index structure), `spotcheck-*.txt`
(validation against the image), `mwvshape.txt`, `dmydata*.txt`, `iso-root-extents.txt`, `hashes.txt`, the scripts
in `work/hd6/scripts/`, and `work/hd6/ghidra/q1..q7.txt` plus decompiled C under `work/hd6/ghidra/src/` written by
the new `tools/ghidra/QueryRefs.java`. The Ghidra project copy is `work/ghidra/s4` (unchanged by the runs). Nothing
below reproduces a name, a byte or a line of code from the disc; the full layout is in `Docs/formats/hd6.md` and
`tools/extract/hd6.hexpat`.

### Facts

**Layout (step 1).**
- The header is 13 little-endian words (52 bytes): magic, then (offset, size, count, 0) for the dictionary,
  (offset, size, 0, size-unit 16) for the path-record table, and (count, offset, 0, file size) for the entry table.
  Dictionary at 0x34; sizes 32,828 / 76,212 / 79,640 bytes in `DATA.HD6` (37,064 / 16,856 / 33,120 in `DATA2.HD6`).
  The brief's first hypothesis (0x34 header, a word equal to the file size, string table at 0x34) was right; the
  entry stride is 8, not 16 — evidence: `probe1-data.txt`, `probe3-data.txt`, `probe3-data2.txt`.
- **Dictionary:** 4,558 NUL-terminated strings in `DATA.HD6` (index 0 and the last one empty; 4,556 real), 4,170 in
  `DATA2.HD6`. They are path *fragments*: a leading name, or a separator (`\`, `.`, `_`, `-`) followed by a run of
  characters. 30 of them are extensions, 2,385 start with `\`. The table is ordered by descending use count (the
  frequency of each index is non-increasing in both files), so the 127 most common fragments get one-byte codes —
  evidence: `probe4-data.txt`, `probe5-data.txt`.
- **Path records:** one NUL-terminated record per entry in entry order; codes are one byte below 0x80, else two
  bytes `(b & 0x7F) | (next << 7)`. All 9,955 + 4,140 records decode with every index in range and every
  dictionary string but index 0 used at least once; records have 1–10 codes and up to 16 bytes — evidence:
  `probe2-data.txt` (the `v7` scheme: 0 failures; the two alternative codings fail on 2,243 and 8,610 records).
- **Entries:** 8 bytes, a 64-bit bitfield: bits 0–17 record offset, 18–39 sector offset in the `.DAT`, 40–63 size in
  16-byte units. The split was first fitted from the data (`probe2`/`probe3`: the only model in which
  `max(offset + size)` lands within one sector of the `.DAT` size for both files) and then confirmed bit for bit by
  the game's lookup code (below) — evidence: `probe3-*.txt`, `work/hd6/ghidra/src/00164570.c`.
- **Validation against the image:** entries are in ascending sector order, members packed back to back on sector
  boundaries, no gaps and no overlaps; the sum of the sector-rounded sizes equals the `.DAT` size exactly for both
  archives (1,804,218,368 and 752,971,776). For **every** member (9,950 non-empty in `DATA.DAT`, 4,139 in
  `DATA2.DAT`) the bytes from `offset + size` to the next sector boundary are zero in the image, and all 4,139
  `DATA2` members start with `RIFF` — evidence: `probe4-*.txt` (walk), `spotcheck-data.txt`, `spotcheck-data2.txt`
  (read through the ISO at `LBA(DATA.DAT) × 2048 + sector × 2048`; LBAs from `iso-root-extents.txt`).
- Oddities: a terminating entry (empty name, sector 0, size 0) ends each table; `DATA.HD6` has 4 zero-size members
  that share the next member's sector and one duplicated path; `DATA2.HD6` has one duplicated path —
  evidence: `probe3-data.txt`, `probe5-*.txt`.
- The magic word 0x00364448 appears nowhere in the ELF as a constant or data word, and the game never checks it
  (`q1.txt`, `const=0x00364448`: 0 hits).

**Reader (step 2).** Addresses in `SLUS_212.07`; names are ours.
- `.HD6;1` (0x003b62b0) and `.DAT;1` (0x003b62a8) are each referenced once, by `archive_open` at **0x00164710**;
  the base names `DATA` (0x003b6298) and `DATA2` (0x003b62a0) are referenced only by `archive_init` at
  **0x00164520**, which is called from the start-up function 0x00160b00 and fills a two-element table of
  `{u32 lsn; void* index}` objects at 0x003e9180 (constructed by the static initialiser 0x003c88e0) and sets the
  archive count at 0x003d26dc to 2 — evidence: `q1.txt` (`str=` queries), `q2.txt`, `src/00164710.c`,
  `src/00164520.c`, `src/003c88e0.c`.
- `archive_open` reads the whole `.HD6` through the file-I/O RPC (`open` 0x0011bba8, `lseek` 0x0011bfb0, `read`
  0x0011c1f0, `close` 0x0011be38) into a buffer of `ceil(size / 2,048) × 2,048` bytes (190,464 B for `DATA.HD6`,
  88,064 B for `DATA2.HD6`, so about 272 KiB of EE RAM for the two indexes), rewrites `\` to `/` in the dictionary,
  and `qsort`s the entry table in place (0x001339f0, element size 8) by the byte order of the encoded records
  (comparator 0x001649d0 → 0x00164a00). It then resolves the `.DAT` with `sceCdSearchFile` (wrapper 0x0012a970;
  the RPC client for server 0x80000597 is bound in 0x0012a670), retrying until success and until
  `sceCdGetError` (0x0012b928) is 0, and stores the file's LSN. The `.DAT` size is never used. A debug print warns
  when the index exceeds 100 KiB, which `DATA.HD6` does — evidence: `src/00164710.c`, `src/001649d0.c`,
  `src/00164a00.c`.
- **Lookup is by encoded name, binary search:** `archive_find` (**0x00164570**) encodes the requested path with the
  archive's dictionary (encoder **0x00164a60**: split at `/ . _ -`, linear `strcmp` scan of the dictionary per
  fragment, one- or two-byte codes exactly as on disc; an unknown fragment fails the lookup), binary-searches the
  sorted entries comparing records byte-wise, and returns `{record offset, lsn + sector, size × 16, 0, 0}`. The
  unpacking in the decompiled code is `(word0 & 0x3FFFF)`, `(byte4 << 14) | (half1 >> 2)` and `(word1 >> 8) ×
  header[0x20]`, which fixes the 18/22/24-bit split and the size unit — evidence: `src/00164570.c`,
  `src/00164a60.c`, `q7.txt`.
- **Read path:** a 12-slot table of 0x7C-byte stream handles at 0x003ee2c0. `open` (0x00161ed0) type 1 = archive
  member (tries `DATA` then `DATA2`), type 4 = ISO file via `sceCdSearchFile` (overlays, movies), type 0 =
  file-I/O RPC, type 2 = host file (its read stub 0x001662b0 returns −1 in the retail build). `read` (0x001611a0
  → 0x00161880) chunks at 1 MiB, converts position and length to sectors and calls **`sceCdRead`** (0x0012b5b8,
  N-command server 0x80000595, function 1; try count 0, spindle control from a handle flag) + `sceCdSync(0)`
  (0x0012ab80) + `sceCdGetError`, retrying every 100 ms on error and re-running the disc check (0x001610b0) on a
  tray-open error. Reads are whole sectors into the caller's buffer — evidence: `src/00161ed0.c`,
  `src/001611a0.c`, `src/00161880.c`, `src/0012b5b8.c`, `q3.txt`.
- The other N-command users are `sceCdSeek` (0x0012b798, function 5, called from the overlay loader 0x00169820 and
  from 0x001cd210) and a disc-state query (0x0012aae8); the S-command binder is 0x0012ac90 and the init binder
  0x0012ae98 (servers 0x80000593 and 0x80000592) — evidence: `q1.txt` (`const=` queries), `q3.txt`.
- **Nothing on the archive path transforms the data:** no XOR, no decompression, no checksum between `sceCdRead` and
  the caller. The S6a movie table is not on it: `.MVI` is not an extension in either index, and the movie code
  (0x0019f2f0) builds its own `cdrom0:\MOVIE\` paths — evidence: `src/00161880.c`, `probe3-data.txt` (histogram),
  `q1.txt` (`str=cdrom0`).
- The ten `lui/ori 0x007FFFFF` sites the mask search found are all libm float code (atan2, pow, sqrt, floor, ldexp,
  printf conversion), not archive code — evidence: `q2.txt`, `src/00125aa8.c` and siblings.

**Extension histogram and the other files (step 3).**
- `DATA.HD6`: 9,954 files, 28 extensions; by count `.chr` 4,454, `.snd` 1,145, `.pak` 696, `.stb` 589, `.cfg` 554,
  `.bin` 515, `.txt` 402, `.map` 338, `.mpk` 334, `.ipk` 321, `.img` 180, `.sky` 112, `.pac` 110, `.mds` 90,
  `.nav` 78, `.str` 7, `.lst` 7, `.mes` 6, `.lgt` 4, `.efp` 3, `.pcl` 2, and one each of `.ico`, `.mot`, `.pac_`,
  `.tm2`, `.bat`, `.tm2pak`, `.cmp`; by bytes `.chr` 732 MB, `.ipk` 265 MB, `.pak` 205 MB, `.mpk` 147 MB, `.snd`
  146 MB. Magic tags where there is one: `.map` `IMG ` / `PCP `, `.sky` `sky2` / `sky0`, `.mds` ` MDS`, `.mot` ` MOT`,
  `.tm2` and `.tm2pak` `TIM2`, `.lgt` `MPL_`, `.pcl` `PCL_`; `.cfg` and `.txt` are text — evidence:
  `probe5-data.txt`, `spotcheck-data.txt`.
- **`DATA2.DAT` is the voice/stream audio archive:** 4,138 `.mwv` + 1 `.wav`, all `RIFF`/`WAVE`; 4,072 mono 32 kHz
  and 66 stereo 48 kHz with format tag 1365 (0x0555; block align 18 and 36; presumably Sony's PS2 ADPCM, to be
  confirmed), the `.wav` PCM stereo 48 kHz; extra chunks `pflt`, `fltr`, `ctrl`, `algn` in 4,138 files, `cmnt` in
  65 — evidence: `mwvshape.txt`.
- **`DMYDATA` is filler:** 51,166,209 bytes of text, 1,054,720 lines of which 4 are distinct, no zero bytes; it is
  the last extent on the disc (LBA 2,016,098) and pads the volume to the full 4,180,148,224-byte DVD. The ELF does
  not reference it (`str=DMY` hits one unrelated debug message) — evidence: `dmydata.txt`, `dmydata2.txt`,
  `dmydata3.txt`, `iso-root-extents.txt`, `q1.txt`.
- Disc layout (root extents): `SYSTEM.CNF` 349, `SLUS_212.07` 350, `DATA2.HD6` 2,119, `DATA.HD6` 2,162,
  `DATA2.DAT` 760,313, `BIN/*` 1,134,883–1,135,131 (from S7), `DATA.DAT` 1,135,132, `DMYDATA` 2,016,098, volume
  2,041,088 sectors — evidence: `iso-root-extents.txt`. SHA-1: `DATA.HD6`
  `5b02cdb216a354328ce2232be7df7134e9109a10`, `DATA2.HD6` `95adef78ce9cff028ceb2ccbc8b76f7200b0aa89`, `DATA.DAT`
  `0edf8dd0b478805e04fe2516dadcdfa07653c252`, `DATA2.DAT` `fe0bcd0cbf2b1f522d498be296c4efec30d68252`, `DMYDATA`
  `29a18218de919a652bc3a566a239095b3e3c62a4` — evidence: `hashes.txt`.

**Tooling.**
- `tools/ghidra/QueryRefs.java` (new, GPL-3.0-or-later): one headless run answers any mix of `str=`, `refs=`,
  `range=`, `const=`, `func=`, `callers=`, `callees=`, `decomp=`, `dis=` and `disrange=` queries against the
  analysed project and appends them to one file; it creates a function on the fly for labelled-only code such as
  the qsort comparator. Runs here took 20–40 s each with `-noanalysis -readOnly -max-cpu 2`.
- `tools/extract/hd6.hexpat` (new): ImHex pattern for the header, dictionary, path records and the entry bitfield.
  ImHex is not installed on this machine, so the pattern is unverified in ImHex itself; its field widths and
  offsets are the ones the Python probes and the decompiled lookup agree on.

### What changes in the docs
- project-brief.md §6, "Game data" row: replace "Index layout not yet decoded (spike S4); no public tooling found"
  with: index = 52-byte header + frequency-sorted fragment dictionary + variable-length-coded path records +
  8-byte entries (18-bit record offset, 22-bit sector, 24-bit size in 16-byte units); 9,954 and 4,139 members;
  `DATA2.DAT` holds the `RIFF`/`WAVE` voice streams; `DMYDATA` is text filler to the end of the DVD; the game reads
  members with `sceCdRead` at `LSN(DATA.DAT) + sector` after a `sceCdSearchFile`; details in `Docs/formats/hd6.md`.
  §6 "What this tells us" item 4: the archive half is done; only the movie table generator (S6b) remains.
- project-brief.md §11, S4 row: done. §12 R6: archive half resolved; residual risk is the member formats
  (`.chr`, `.pak`, `.ipk`, `.mpk`, `.snd`, `.stb`), which the port does not need to parse to run the game.
- architecture.md §3 step 2: `not_extracted` for the `.DAT` files confirmed; the `.HD6` files are optional
  tooling inputs. §5.1/§5.3 (HLE services): the CD path the game uses is `sceCdSearchFile` once per archive,
  then `sceCdRead` of 1–512 sectors with `sceCdSync(0)` and `sceCdGetError` polling, plus `sceCdSeek` from the
  overlay loader; the file-I/O RPC (`open/lseek/read/close`) is used for the `.HD6` files at boot. §8.3: the disc
  model needs random sector access by LSN and the ISO directory; latency modelling can key on (LSN, count). §14:
  add the HD6 dictionary and path records to the game-derived values that never enter the repository or tool
  output.
- tech-stack.md §7: ImHex row: `tools/extract/hd6.hexpat` is the first committed pattern; Ghidra row: add
  `tools/ghidra/QueryRefs.java`.
- `Docs/spikes/S6-mvi-movie-format.md` open question 1 and the S6b brief: the generator does not touch the
  `DATA.DAT` read path; look for it in the movie player only.
- `data/regions/SLUS_21207.toml`: optionally record the `.HD6` sizes and hashes above (sizes and hashes are
  allowed) so `dq8ctl` can verify them before any archive tooling runs.

### Decision impact
- No decision in brief §14 changes. D3 (local game-module build) is unaffected: nothing in the archives is needed
  by the recompiler. D7 (NTSC-U first): the layout is build-independent, so other regions will differ only in the
  hashes and LBAs.
- Proposal for the M1/M2 tooling backlog, not a decision: a `dq8ctl archive list|extract` subcommand over
  `Docs/formats/hd6.md` (walk entries, decode names on the user's machine, read `lba + sector`), useful for the
  sound and movie work, never on the boot path.

### Local artifacts (git-ignored)
- `work/hd6/scripts/probe1.py … probe5.py`: index structure probes (counts only on stdout). `spotcheck.py`: every
  member's tag and padding checked against the image. `mwvshape.py`: RIFF shape of `DATA2` members. `zerocheck.py`,
  `dmyprobe.py`, `dmylines.py`: `DMYDATA` characterisation. `isoextents.py`: root-directory extents.
- `work/hd6/probe*-data*.txt`, `spotcheck-*.txt`, `mwvshape.txt`, `dmydata*.txt`, `iso-root-extents.txt`,
  `hashes.txt`: their outputs.
- `work/hd6/ghidra/q1.txt … q7.txt` + `.log`: the seven `QueryRefs.java` runs; `work/hd6/ghidra/src/*.c|.asm`:
  decompiled and disassembled functions (game-derived, stays here).
- `work/ghidra/s4/`: copy of `work/ghidra/base`, opened read-only; can be deleted.

### Open questions
- Member formats: `.chr` (4,454 files, 732 MB, no single magic), `.pak`/`.ipk`/`.mpk` containers, `.snd`, `.stb`,
  `.nav`, `.map` (`IMG `/`PCP `), `.sky`; none is needed to boot → M4+ (modding and asset tooling), or whenever a
  feature (texture replacement, widescreen UI) needs one.
- Format tag 0x0555 in the `DATA2` streams and the `pflt`/`fltr`/`ctrl`/`algn` chunks: which IOP module decodes them
  and whether it is Sony ADPCM → M5 (sound stack), together with the `.snd` format.
- The two stored 256-byte path prefixes (0x00394e30, 0x00394f30) and the `<drive>:` prefixes the game passes to
  `open`: which names the game uses for archive members versus ISO files → M2, when the HLE of the file-I/O RPC is
  written; `QueryRefs.java str=` on the suspected prefixes answers it in one run.
- The 100 KiB warning threshold suggests the index was expected to fit a fixed budget; whether the retail
  allocation (`malloc` 0x00100860) comes from the main heap or a dedicated pool matters for the memory map → M2.
- Whether `sceCdRead`'s spindle-control flag (handle flag bit 0) is ever set for archive members (streaming
  audio) → S7 or M3, from a PCSX2 trace of the CDVD commands during play.
