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

## Update from spike S2 (2026-10-19)

- Copy `work/ghidra/base` (not `plain`): it carries the `bss` and `overlay_window` uninitialized blocks that
  `tools/ghidra/MapElfSegmentTails.java` adds, because Ghidra's ELF loader silently maps none of the zero-file-size
  segments. The project already holds **120 one-byte function entries inside the window and 374 references into
  it** (`work/ghidra/base/census-base.csv`): that is your starting list for questions 1, 4 and 5.
- To analyse overlay code with cross-references in both directions, work inside a copy of the main program rather
  than importing the `BIN` as a separate program: write `tools/ghidra/LoadOverlay.java` (postScript with the BIN path
  as its argument) that converts the `overlay_window` block to initialized memory, writes the whole file at
  `0x00461C80` (header included, code at `0x00461CC0`), creates functions at the `table_start..table_end` entries
  and at the 120 known entry points, and re-runs analysis. One project copy per overlay (`work/ghidra/s3-title`,
  `s3-battle`, …). Headless import and analysis of the main ELF takes 2–3 minutes, not hours; `-analysisTimeoutPerFile 600` is plenty.
- The 78 `syscall` sites and their `$v1` numbers are in the census; filter for `0x64` and `0x68` (question 3).
- The main program has 31 computed `jr` sites without a resolved switch table and 993 `jalr` sites without a static
  target; note which of them sit on the path into the window.

## Findings

**Session:** 2026-10-19, about 40 minutes wall time (phase 1 read-only queries 20:45–21:00, phase 2 and 3 imports and
checks 21:00–21:12, the rest writing). **Stop reason:** done.

All numbers come from the files under `work/s3/` and `work/ghidra/s3-*/` listed at the end. They are addresses, counts,
sizes, flags and Ghidra's own auto-names; nothing from the disc is reproduced here. "Main" means the boot ELF's code;
"window" means `0x00461C80`–`0x00489A7F`.

### Facts

**1. The loader (brief Q1).**
- **One function loads an overlay: `0x00100330`, signature `int load(const char* path, void* dest)`**, 272 bytes, two
  callers. It returns 1 on success and 0 on any failure (open failed, size not positive, read not positive), and both
  callers spin on it until it returns 1: there is no error path. Evidence: `work/s3/src/00100330.c`, `.asm`;
  `work/s3/phase1b.txt` section `func=100330`.
- **Load by index: `0x00169860`, signature `int load_overlay(int index)`**, 148 bytes, 10 callers. Index 0–5 selects an
  entry of a **six-pointer name table at `0x003950F0`** (initialized data) whose order is **TITLE, CASINO, VIEWER, BATTLE,
  MENU, SHOP** (this is not the `overlay_id` order of the headers: CASINO 1, VIEWER 2, SHOP 3, MENU 4, BATTLE 5,
  TITLE 6). The six pointers are the six `BIN\` path strings the brief counted (they are also six of the ten `cdrom0:`
  strings; the loader opens by full device path). Any other index returns 1 without loading. The destination it passes
  is **the data word at `0x003D1704`**, which holds `0x00461C80`: the main program never materialises the window
  address in code (`const=461C80`: 0 register pairs, 1 data word), it reads this pointer (4 reads, all in the two loader
  callers). Evidence: `work/s3/src/00169860.c`; `work/s3/phase1.txt` sections `str=BIN\`, `const=0x00461C80`;
  `work/s3/phase1b.txt` sections `refs=3d1704`, `range=3950f0-395107`.
- **Thirteen on-demand load sites in 10 functions** pass a constant index to `0x00169860`: BATTLE (index 3) from
  `0x0017CDD0`, `0x0036F3F0`, `0x0036FA70`, `0x0037FBD0`, `0x00380100`; MENU (4) from `0x0036EFC0` (twice),
  `0x0036F3F0`, `0x0036F710`; SHOP (5) from `0x0036EFC0`, `0x0037FDC0`, `0x00380300`; CASINO (1) from `0x002CDD30`.
  The BATTLE reloads are the returns from menu, shop and casino to the field. Evidence: `grep 'FUN_00169860(' work/s3/src/*.c`.
- **The game is a 12-mode state machine whose main loop lives in `0x001693B0`** (916 bytes, called once from the main
  routine `0x00160B00`). The mode is a 16-bit variable at **`0x003D2740`** (next mode `0x003D2744`, previous
  `0x003D2748`; all three in bss, read and written only by this function). On every mode change the loop first loads
  the overlay the mode needs, **directly through `0x00100330`** (not by index): modes 1, 2, 4, 5, 6, 9, 11 load
  TITLE; modes 3, 8, 10 load VIEWER; every other mode (0 and 7) loads BATTLE. It then calls the mode's init function,
  runs its update function until it returns non-zero, and calls its exit function, through **three 12-entry pointer
  tables at `0x00395060` (init), `0x00395090` (update) and `0x003950C0` (exit)**. Mode 0's three functions are in main
  (the field); the other 33 table entries point into the window. Evidence: `work/s3/src/001693b0.c`;
  `work/s3/phase1c.txt` sections `refs=3d2740`, `refs=3d2744`, `range=395060-3950ef`.
- **Descriptor table:** there is none beyond the name table. The game-loop init `0x001690E0` (720 bytes, called once)
  resolves each of the six names with `sceCdSearchFile` (wrapper `0x0012A970`, after stripping the device prefix) and
  stores the six **start sectors in a six-word cache at `0x003F8850`** (bss). The title save state holds exactly
  S7's ISO extents there: 1135078, 1134960, 1135131, 1134883, 1135024, 1135089 (TITLE, CASINO, VIEWER, BATTLE, MENU,
  SHOP). Evidence: `work/s3/src/001690e0.c`; `work/s3/state/title-peek.txt`.

**2. Read path (brief Q2).**
- Both callers first call **`0x00169820`, `void preseek(int index)`**, which issues `sceCdSeek` (`0x0012B798`, the
  cdvd N-command server, RPC id `0x80000595`) to the cached start sector. Then `0x00100330` reads the file **through the
  file-I/O RPC (`0x80000001`)**: `open(path, O_RDONLY)` `0x0011BBA8`, `lseek(fd, 0, SEEK_END)` and `lseek(fd, 0,
  SEEK_SET)` `0x0011BFB0`, **one `read(fd, dest, size)`** `0x0011C1F0` of the whole file (header included, exactly the
  file size, not sector-rounded), `close` `0x0011BE38`. These are the same four wrappers S4 found for the `.HD6`
  reads. So the pre-seek is a latency hint only; the data path is the IOP's file system by name. Evidence:
  `work/s3/src/00100330.c`, `00169820.c`; `work/s3/phase1.txt` sections `const=0x8000000x`, `callers=12b798`.
- The read lands at `dest` = `0x00461C80`, so the header is resident and code begins at `0x00461CC0`, as the brief's
  size arithmetic predicted. The title save state confirms it: magic intact at the window base, `overlay_id` 6,
  `load_address` field `0x00461C80`, text/data/bss `0x4AC0`/`0x680`/`0x600`, table `0x00466DA0`–`0x00466DB8`, name field
  lowercase ASCII. Evidence: `work/s3/state/title-peek.txt` (from `work/pcsx2/boot-title-20261019-200258/title.p2s`,
  read with `work/s3/scripts/peek_state.py`; the EE extension's importer was not needed because the window block is
  executable and the importer skips executable blocks).

**3. Destination and post-processing (brief Q3).** After the read, `0x00100330` does, in this order:
1. calls **`0x00100320(dest)`, an empty function** (a return and a delay-slot nop; the only call site). It is a hook
   point the developers compiled out and the cleanest place for ours;
2. reads `bss_size` from the header (`dest + 0x14`);
3. **`FlushCache(0)` then `FlushCache(2)`** (write back the data cache, invalidate the instruction cache): the pair
   the brief predicted, through the kernel-call stub at `0x00116AA0` (syscall `0x64`; the census has that one site and
   no `0x68`/`iFlushCache` site at all). The stub has 24 callers; only this pair makes overlay code live;
4. **zeroes the overlay's bss** with `0x00132848` (memset by its arguments, *inferred*) at `dest + file_size` for
   `bss_size` bytes, i.e. bss starts
   at `64 + text + data` (the file size, taken from `lseek`, not from the header's sizes);
5. calls **`0x00100DE0(table_start, table_end, 0, 0)`** with the header's two table fields (`dest + 0x18`, `+0x1C`).
   Nothing above the new overlay's memsz is cleared: bytes of a larger earlier overlay stay in the window.

   The fields the game reads from the header are therefore **only `bss_size`, `table_start` and `table_end`**. Magic,
   `overlay_id`, `load_address`, `text_size`, `data_size` and the name are never read by the game (`const=0x336F574D`:
   0 register pairs, 0 data words; no load from `dest + 0x0/0x4/0x8/0xC/0x10/0x20` in the loader). Evidence:
   `work/s3/src/00100330.asm`, `00100320.asm`.
- **Boot state of the window:** the ELF entry (`0x00100008`) zeroes memory from the bss start `0x003D2680` to
  `0x00489A80`, so the window starts zeroed, and passes `0x00489A80` as the end of the image to the start-up helper
  `0x00120B98` (*inferred*: the SDK thread/heap setup). The heap-break word at `0x00390F74` (read and written only by
  `0x00116F20`, next to the kernel stubs; *inferred* to be the SDK's `sbrk` pointer) is initialised to `0x00489A80`:
  **the heap begins exactly at the window end**, nothing else is ever placed in the window. Evidence:
  `work/s3/src/00100008.c`; `work/s3/phase1.txt` section `const=0x00489A80`; `work/s3/phase1b.txt` `refs=390f74`.

**4. The table (brief Q4): a C++ static-constructor list.**
- `0x00100DE0(begin, end, a2, a3)` walks the pointer array from `begin` to `end` and **calls every entry with no
  arguments through `jalr`**, then calls `0x00102770(a2, a3)`, an empty function returning 0. It has exactly two
  callers: the overlay loader, and `0x00100450`, which the main routine calls once with **the main program's own
  constructor table `0x003CF910`–`0x003CFB80` (156 entries)** and `0x003D1700` twice as the extra arguments. So the
  overlay table is the same kind of table as the main program's. Evidence: `work/s3/src/00100de0.asm`, `00100450.c`,
  `00102770.c`; `work/s3/phase1c.txt` section `callers=100de0`.
- Each entry is a short thunk that constructs global objects and registers their destructors with `0x00100DB0`
  (object, destructor in main, a slot in the overlay's bss). **The thunks sit in the overlay's *data* range, after the
  text**: TITLE's six thunks lie between `0x00466B00` and the table itself at `0x00466DA0`, while its text ends at
  `0x0046677F`. Ghidra found 150 (TITLE), 327 (CASINO), 84 (VIEWER), 113 (BATTLE), 97 (MENU) and 247 (SHOP)
  instructions in the data ranges, reached from the tables and from what the thunks call. So the header's `text_size`/`data_size` split is a link-map boundary, not a code/data
  boundary, and the function map must be allowed to put code anywhere in `64 + text + data`. Evidence:
  `work/s3/src-title/00466b00-00466b5f.asm`; `window_instructions_in_data_range` in `work/ghidra/s3-*/overlay-*.csv`.
- Entry counts: TITLE 6, CASINO 5, VIEWER 4, BATTLE 5, MENU 4, SHOP 3 (as the brief read from the headers). All are
  called once per load, in order, before the loader returns. Nothing else references the tables.

**5. Entry mechanism and inter-unit calls (brief Q5).** The 374 references into the window are:
- **168 direct `jal` sites in 43 main functions, to 120 distinct targets** (the 120 one-byte entries from S2). Seven
  battle-side callers account for 109 of them (`0x002A3A00` 33, `0x002AB2B0` 20, `0x002A9E20` 17, `0x002A2C10` 14,
  `0x002A75D0` 14, `0x002AAAC0` 6, `0x002AACC0` 5). The main program was linked against each overlay's addresses:
  calls into overlay code are ordinary absolute calls, valid only while the right overlay is resident.
- **202 `DATA` references**: 6 are the ELF section-header words; 72 are words in the main image, of which 63 have an
  aligned target (the window pointer at `0x003D1704`, 33 entries of the three mode tables, 19 in
  `0x003D135C`–`0x003D165C`, 8 in `0x003D2594`–`0x003D25B0`, 1 at `0x00398C4C`, and 1 at the odd source address
  `0x0039EEC6`, probably not a pointer) and 9 have an odd target (words in string and float data, not pointers); the
  remaining 124 are the EE extension's uncached and accelerated mirrors of the 62 aligned words below the window
  pointer. The pointer tables are called through `jalr` or read as data by main code and are the other entry
  mechanism. Evidence: `work/s3/main-data-pointers.txt`;
  `work/s3/phase1.txt` section `range=461C80-489A7F`.
- 4 `PARAM` references: the loader's two callers passing the window pointer.
- **Calls from each overlay into main** (after `LoadOverlay.java`, counts of `jal` sites and distinct targets; the
  table also has the other per-unit measures):

| measure | TITLE | CASINO | VIEWER | BATTLE | MENU | SHOP |
|---|---|---|---|---|---|---|
| functions found in the window | 69 | 226 | 16 | 624 | 258 | 395 |
| instructions in the window | 4,845 | 25,443 | 166 | 35,644 | 15,418 | 11,725 |
| text bytes inside function bodies / text size | 18,708 / 19,136 (98%) | 95,920 / 110,016 (87%) | 328 / 576 (57%) | 141,604 / 146,112 (97%) | 55,504 / 103,872 (53%) | 43,040 / 61,504 (70%) |
| calls to main (sites / distinct targets) | 695 / 284 | 1,736 / 204 | 32 / 13 | 2,255 / 365 | 1,744 / 506 | 1,283 / 310 |
| unconditional jumps to main (tail calls) | 0 | 6 | 0 | 7 | 5 | 8 |
| calls inside the overlay | 51 | 767 | 0 | 1,062 | 171 | 472 |
| data references to main image / to bss | 44 / 424 | 332 / 1,504 | 8 / 16 | 252 / 1,068 | 144 / 565 | 93 / 270 |
| `jalr` / computed `jr` / computed `jr` without a table | 4 / 0 / 0 | 64 / 10 / 0 | 0 / 0 / 0 | 62 / 7 / 0 | 24 / 5 / 1 | 13 / 0 / 0 |
| syscall sites | 0 | 0 | 0 | 0 | 0 | 0 |
| references beyond the overlay's own memsz | 0 | 0 | 0 | 0 | 0 | 0 |
| main call targets that are a function entry here | 20 | 44 | 0 | 109 | 62 | 61 |

- Overlays contain **no syscalls** and never call the loader, the mode machine or `FlushCache`: in all six projects the
  callers of `0x00169860`, `0x00100330`, `0x001693B0` and the `FlushCache` stub are main functions only, and no
  overlay instruction references `0x003D2740`, `0x003D2744`, `0x003D2748`, `0x003D1704` or `0x003F8850`. **Overlays
  never load overlays.** Evidence: `work/s3/q3-*.txt`.
- **Which overlay a call site belongs to.** A target address is often a function entry in several overlays (the six
  overlays share only their first 64 text bytes, which are 16 `nop`s, and only about 1% of equal words at equal
  offsets, so this is coincidence of boundaries in dense code, not shared code). Voting per caller over all its sites
  attributes **129 of the 168 sites to BATTLE alone, 8 to MENU, 3 to SHOP, 2 to CASINO**; 22 sites in 17 small
  callers are consistent with two or more overlays and 4 (`0x0017CDD0`, the field function that reloads BATTLE)
  with none under the strict boundary test. Per-site attribution therefore needs the caller's execution context,
  which is M2's function-map work. Evidence: `work/s3/entries-*.csv`, `python3 -I work/s3/scripts/attribution.py`.
- Mode-table attribution agrees with the loop's switch: the init/update/exit entries of modes 3, 8 and 10 are
  function entries in VIEWER for all 9 slots (and in no other overlay for 5 of them); those of modes 1, 2, 4, 5, 6, 9
  and 11 are entries in TITLE for all 21 slots. The first function of every overlay is at `0x00461D00` (after the 64 bytes of
  `nop` padding at `0x00461CC0`); it is a function entry in all six overlays and is mode 5's and 7's update function.

**6. Self-modifying code (brief Q6): none.**
- Main program: 4,863 write references in total, **0 whose target lies inside any instruction and 0 into the
  window**; the only writes into the window are the file-I/O RPC's DMA. The 17 `cache` instructions of the census sit
  in three SDK functions (`0x00117070` 4, `0x001171B0` 4, `0x00119D30` 9: SIF buffer maintenance, not code patching).
  Evidence: `work/s3/smc-main.csv`.
- Overlays: 0 write references into window code or main code from any of the six, and 0 from main into the window
  (`window_write_refs_into_*`, `main_write_refs_into_window` in every `overlay-*.csv`). The runtime may compile all
  seven code units ahead of time and treat the window as read-only code between loads.

**7. Resident-overlay variable (brief Q7): there is none.** The loader stores nothing; the mode variable
`0x003D2740` plus the fixed mode-to-overlay switch, and the 13 constant-index load sites, are the only record. At
the title screen the state has mode 4 with TITLE resident. The header at the window base (and `overlay_id` at
`window + 4`) is the only in-memory identification, and it survives until the next load because nothing writes
the window. Evidence: `work/s3/state/title-peek.txt`.

**Stale returns.** One chain of at most three calls leads from overlay code (the TITLE project's function at
`0x00465530`, a multi-owner address) through main to the CASINO load site `0x002CDD30`; no chain reaches the other
nine load sites from any overlay within that depth. A swap while an overlay frame is on the stack cannot be excluded
statically (the game reloads the same overlay afterwards, so it works on hardware), which keeps architecture.md's
generation counter justified. Evidence: `work/s3/q3-title.txt` section `callers=2cdd30`.

**Tooling.** `tools/ghidra/LoadOverlay.java` (postScript, arguments: BIN path and report CSV) converts the
`overlay_window` block to initialized memory, writes the whole file at the header's load address, applies a header
structure and the pointer array, and seeds code in four passes with analysis after each: constructor thunks; the
main program's call targets and placeholder entries where they land on undiscovered bytes; pointers from main data;
pointers from the overlay's own data range (vtables), which raised BATTLE's text coverage from 43% to 97%, SHOP's from
25% to 70% and MENU's from 33% to 53%. Each import takes 4–18 s with `-max-cpu 4` under `nice -n 19`; the read-only
query runs take 7–25 s. Two caveats: Ghidra's reference analyzer creates a function at every main call target inside
the file range, so four of them overlap real CASINO functions (logged as "function body repair failed" at
`0x00465AE0`, `0x00465B30`, `0x00465B80`, `0x00467F50`), and the analysis in the SHOP project changed two main
functions so that `0x00169860` shows 8 callers there instead of 10. Both are cosmetic for a spike and must be handled
by the M2 map generator (seed from attributed targets only).

### What changes in the docs
- project-brief.md §6: overlay-container row: replace "semantics to be confirmed by spike S3" with: the game reads only
  `bss_size`, `table_start`, `table_end`; the destination comes from the word at `0x003D1704`; the table is a C++
  static-constructor list whose thunks lie in the data range; the first 64 text bytes are `nop` padding. Overlay-window
  row: loader `0x00100330`, load-by-index `0x00169860`, name table `0x003950F0`, sector cache `0x003F8850`, mode variable
  `0x003D2740`, heap starts at `0x00489A80`, no self-modifying code anywhere. Add the per-overlay function and
  instruction counts from the table above. §11: S3 row done; M1 part 2 unblocked. §12 (R1): risk reduced to "design
  confirmed; per-site attribution and MENU's 53% coverage are M2 work".
- architecture.md §4.3: rewrite the "Know when an overlay becomes resident" row: **primary: replace `0x00100330` with a
  native implementation** (read the file from the image into guest RAM at `dest`, tell the manager which overlay by
  the name-table index or the resident `overlay_id`, repoint dispatch, then run the constructor table through dispatch);
  **cheaper variant: hook the empty `0x00100320(dest)`** (called after the read, before the flush), which is exactly
  "bytes are in RAM, code is not yet live"; fallback as written (read `overlay_id` at `window + 4`; the header stays
  resident and nothing writes the window). Add: the constructor walk `0x00100DE0` calls overlay code through `jalr`
  from main, so the window's dispatch range must be switched **before** the loader returns, not after. Add: overlays
  never load overlays, contain no syscalls, and the mode machine in `0x001693B0` is the only switch logic. Add the
  "no self-modifying code" verdict to §4.3 and §4.6. State that code exists in the overlays' data ranges and that the
  per-unit function maps decide what is code. §5.1: `FlushCache(0)`/`FlushCache(2)` are the only cache calls on the
  path and are no-ops for us; the file-I/O RPC (`open/lseek/read/close` by `cdrom0:` path) must be HLE'd before the
  first overlay load, which happens before the title screen; `sceCdSeek` can be a no-op. §8.3: the window ends where
  the heap begins. §13.4: add `window_pointer`, `loader`, `load_by_index`, `name_table`, `lsn_cache`, `mode_variable`
  to the region-profile schema (see "Decision impact"). §18: ADR-0003's premise (no overlay support upstream) stands.
- tech-stack.md §7: add `tools/ghidra/LoadOverlay.java` with its four-pass seeding and timings; note that the
  save-state importer skips executable blocks and that a 40-line Python reader of the stored `eeMemory.bin` entry was
  enough.
- Docs/spikes/README.md and the S2 open questions: the 120 entries and 374 references are explained above.

### Decision impact
- **M1 part 2 (the `MWo3` parser) can proceed with these semantics:** validate magic, `load_address == window base`,
  `file_size == 64 + text + data`, `memsz == file_size + bss == the ELF segment's memsz`, `table_start..table_end`
  4-aligned inside `[64 + text, 64 + text + data)`; expose `image = text + data` as one byte range for the recompiler
  and `bss` as a size; treat `text_size` as a hint only. Add to the region profile per overlay: `index` (loader
  index), `unit` name, `segment_index` (ELF PT_LOAD 1–6), `memsz`, and the expected header fields as integrity values
  (`text_size`, `data_size`, `bss_size`, `table_entries`); and to `[overlay_window]`: `code_offset = 0x40`,
  `window_pointer = 0x003D1704`, `loader = 0x00100330`, `load_by_index = 0x00169860`, `name_table = 0x003950F0`,
  `name_table_order = ["TITLE","CASINO","VIEWER","BATTLE","MENU","SHOP"]`, `lsn_cache = 0x003F8850`,
  `ctor_walker = 0x00100DE0`, `loader_hook = 0x00100320`, `flush_cache_stub = 0x00116AA0`, `mode_variable = 0x003D2740`.
- **OverlayManager handshake (M2/M3):** (1) the native loader override reads the overlay from the image into guest RAM
  at `dest`, (2) calls `manager.resident(index or overlay_id, generation+1)`, which repoints the window's dispatch slots
  to that unit's table and bumps the generation, (3) zeroes bss, (4) runs the constructor table through dispatch
  (the generated main-program code for `0x00100DE0` does this by itself if the override returns to guest code after
  step 2), (5) returns 1. The dispatcher must model the table calls only in the sense that the `jalr` in `0x00100DE0`
  goes through dispatch like any other indirect call; no special table handling is needed. `FlushCache` stays a
  no-op. Debug builds verify `overlay_id` at `window + 4` against the manager's record at every dispatch into the
  window.
- D4 / ADR-0003: unchanged; the per-unit recompile is confirmed as the right shape (no SMC, fixed window, 43 main
  callers with absolute targets). ADR-0001/0002 (dispatch design): the window needs one dispatch range of 163,328
  bytes / 4 = 40,832 slots at most; the 120 main targets, 63 pointer-table targets and each unit's internal
  `jalr`/`jr` (4–64 per unit) are the only entries it must resolve.
- R1: closes as "understood"; the residual risk moves to M2 (function discovery in MENU and the four overlapping
  CASINO entries).

### Local artifacts (git-ignored)
- `work/ghidra/s3-main/`: read-only copy of `base` used for all phase-1 queries.
- `work/ghidra/s3-{title,casino,viewer,battle,menu,shop}/`: one copy per overlay with the overlay loaded and analysed
  by `LoadOverlay.java`; `overlay-<name>.csv` in each is the report (counts, plus one row per main target with how
  it lands in that overlay).
- `work/s3/phase1*.txt`, `.log`: QueryRefs output (string hits, constants, the 374 references with their sources,
  caller trees, function summaries); `work/s3/src/*.c`, `*.asm`: decompiled and disassembled loader cluster (game-derived,
  never to be copied); `work/s3/src-title/`, `src-ov/`: overlay listings.
- `work/s3/smc-main.csv`: the main-program write-reference scan; `work/s3/entries-*.csv`: per-overlay entry check;
  `work/s3/q3-*.txt`, `phase3-*.log`: per-copy loader-caller and mode-variable queries; `work/s3/main-data-pointers.txt`.
- `work/s3/state/title-peek.txt`: the save-state reading (header fields, pointer, mode, sector cache, bss and tail
  zero counts).
- `work/s3/scripts/`: `phase1*.sh`, `phase2.sh`, `phase3.sh`, `smc-main.sh` (runners), `SmcScan.java` and
  `EntryCheck.java` (scratch Ghidra scripts, candidates for promotion to `tools/ghidra/` in M2), `peek_state.py`,
  `crosstab.py`, `attribution.py`.
- `work/s3/load-*.log`: the six import logs (each ends with `END exit= wall=`).

### Open questions
- MENU's text coverage is 53% and VIEWER's 57% after four seeding passes; the remainder is probably reached through
  pointers in rodata inside the text range or through data in main. A pattern-based function-start search per unit, or
  seeding from every aligned word in the overlay image that points into its text, is the next step → M2.
- Per-site attribution of the 39 ambiguous main call sites and the 20-plus pointer tables in `0x003D13xx`–`0x003D25xx`
  needs the caller's mode context (which overlay is resident when that main function runs) → M2 function maps.
- What `0x00100DB0` registers (object, destructor, slot) and whether the destructor list is ever run with an overlay
  unloaded (it would call main destructors on overlay objects) → M3, when exit paths are implemented.
- Whether the mode-7 overlay (BATTLE by the default branch) is also what the field (mode 0) relies on for its 129
  attributed call sites, i.e. whether BATTLE is resident during all field play → confirm with a field save state (S7
  has `field.p2s`) in M2.
- The 10 on-demand callers: which guest thread they run on and whether the loader can be re-entered from the
  `0x00169790` worker thread → M3 threading.
