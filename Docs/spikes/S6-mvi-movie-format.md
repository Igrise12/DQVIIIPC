# S6: What is the .MVI movie container?

**Time box:** 2 sessions (S6a now, S6b after S2). **Answers:** brief Q6, risk R6, tech-stack §5 decoder choice.

## What is already known

38 `.MVI` files under `MOVIE/`, mostly in pairs `X.MVI` / `X_W.MVI` (`_W` very likely a 16:9 variant; this
also matters for brief §7's widescreen row), including two 248 MB files `MOVIE/EV/URA.MVI` and `URA_W.MVI`.
The boot ELF contains four `sceMpeg` strings and one `.MVI` string. `904_A1.MVI` and `904_A1_W.MVI` share
their first 64 bytes; `904_A2` and `904_B1` do not share them. Nothing more is established: the usual PS2 route
is MPEG-2 decoded by the IPU, but the container is unidentified.

## S6a (independent; scratchpad only; never extract a movie into the repo)

Stream with `7z e -so "$DQ8_ISO" MOVIE/E904/904_A1.MVI | head -c N | python3 -I script.py` from a directory
that holds no untrusted files.

1. Entropy per 64 KB window over `904_A1.MVI` and one short `P*.MVI` (uniform ~8 bits/byte throughout means
   compression or obfuscation; a lower-entropy header region means a plain container).
2. First differing offset between `904_A1.MVI` and `904_A1_W.MVI`, and between unrelated files; sizes of the
   `_W` pairs (a constant ratio suggests resolution, a constant delta suggests metadata).
3. Known-container tests on the first 4 KB: MPEG-2 program stream (pack header `00 00 01 BA`), Sony PSS,
   `sceMpeg` demux header shapes, IPU bitstream; look for a repeating 16-byte or 2048-byte period (sector
   interleave of video/audio).
4. If a plain or trivially transformed container appears, `ffprobe` the first 1 MB from stdin
   (`ffprobe -f mpeg -i pipe:0`) and record codec, resolution, frame rate, audio stream ids.

## S6b (after S2 import; copy `work/ghidra/base` to `work/ghidra/s6`, or REA if S0 passed)

Xrefs from the `sceMpeg` strings; references to the IPU registers (`0x10007000`–`0x10007030`) and the IPU DMA
channels (`0x1000B000` = channel 3, from-IPU; `0x1000B400` = channel 4, to-IPU; the brief originally had them swapped); the `sceMpeg` data-supply callback is where any
container parsing or transform lives. Describe the algorithm in prose; record the **addresses** of any
constants, never their values (tools and the runtime read such constants from the user's ELF through the
region profile, consistent with the shape-only rule). Confirm whether audio is interleaved (private stream,
ADPCM) or separate.

## Deliverables

- `Docs/formats/mvi.md`: container layout (field tables), inner codecs, `_W` meaning, audio layout.
- Findings: decoder decision for tech-stack.md §5 (FFmpeg LGPL `mpeg2video` + our demux and ADPCM, or
  libmpeg2), architecture.md §7.2 confirmation, brief §7 widescreen note.

## Must not commit

Any movie bytes, decoded frames, key or constant values.

## Findings

**Session:** 2026-10-19, about 50 minutes of scripted analysis plus the write-up (S6a only). **Stop reason:** done for
S6a; S6b waits for the S2 Ghidra project. All evidence paths below are under `work/mvi/` (git-ignored); the scripts
are in `work/mvi/scripts/` and run with `python3 -I`, movie bytes only ever flow through pipes.

### Facts

**Container**

- All 38 files have size `4 + N × 16384` (N from 213 to 15219) — evidence: `sizes.txt`.
- Every `.MVI` is a standard **MPEG-2 program stream** (ISO/IEC 13818-1) **XORed, from file offset 0, with a fixed
  2048-byte table** that repeats every 2048 bytes (the CD-ROM sector size). Under the table every 16 KB block starts
  with a pack header, every block is consumed exactly by program-stream syntax, and the trailing 4 bytes are the
  MPEG program end code — evidence: `key_final_verify.txt` (234/234, 213/213, 426/426 and 512/512 blocks of four
  files), `survey_all.txt` and `decode_all.txt` (all 38 files).
- **One table for all 38 files**: recovered independently from six files it agrees 2048/2048 between the two
  cleanest sources, and all 38 files parse and decode with it — evidence: `key_recovery.txt`, `decode_all.txt`.
- Before the XOR the usual candidates are all ruled out: entropy 7.80 to 7.997 bits/byte in every 64 KB window with
  no lower-entropy header region, zero `00 00 01` start-code prefixes in 3.8 MB (chance expectation 0.2), no RIFF,
  PSS, CRID, Sofdec, SShd or VAG tags — evidence: `scan_904_A1.txt`, `scan_904_B1.txt` (brief steps 1 and 3).
- How it was found (brief step 3): byte-equality rates are elevated at every multiple of 256 and peak at 2048
  (`period_904_A1.txt`); ciphertext bytes at the same offsets of every 16 KB block are constant exactly where an
  MPEG-2 pack header plus PES header would sit (`scan_904_A1.txt`, "distinct values per byte position"); per-residue
  frequency analysis over period 2048 then yields pack headers in 234/234 blocks (`trykey_904_A1.txt`); the exact
  table is the most-repeated ciphertext sector, 572 identical copies across four files, which can only be a constant
  plaintext sector (digital-silence PCM) — `key_refine.txt`, `key_final_verify.txt`.

**The table**

- 2048 bytes, 255 distinct byte values, entropy 7.80 bits/byte (random-looking). Read as 512 little-endian 32-bit
  words it is **exactly the output sequence of a 32-bit linear congruential generator**, `x[n+1] = a·x[n] + c mod
  2^32`, verified over all 511 transitions after lifting 8 bits at a time; four algebraically equivalent `(a, c)`
  solutions exist, one with `c = 0` (a multiplicative generator with `a ≡ 5 mod 8` and an odd seed). The first word
  of the table is the seed. The multiplier is none of the textbook constants (ANSI C, MSVC, Borland, Delphi,
  Park–Miller, RANDU, Knuth, Java) — evidence: `lcg_fit.txt`, `lcg_known.txt`, `table_structure.txt` (the low byte
  lane repeats every 64 words, as the low bits of a power-of-two LCG must).
- The table, and its byte-swapped form, occur **nowhere on the disc**: not in `SLUS_212.07`, the six overlays, the
  16 IRX modules, `IOPRP300.IMG`, `DATA.DAT`, `DATA2.DAT` or the HD6 indexes (`DMYDATA` was not searched). So the
  game generates it at runtime from two or three constants (seed, multiplier, increment or none) — evidence:
  `find_table.txt`, `find_table_disc.txt`.
- The table and its constants are game-derived and stay out of the repository and out of tool output. The runtime
  can obtain the table either from the user's ELF through the region profile (addresses from S6b) or by regenerating
  it from the user's disc at setup with the most-repeated-sector method (`scripts/mvi_key3.py`, about a minute).

**Program-stream layout** (identical scheme in all 38 files; `survey_all.txt`, `pt_stats_904_A1.txt`,
`audio_904_A1.txt`)

| Field | Size | Notes |
|---|---|---|
| Pack header | 14 bytes at every 16384-byte boundary | MPEG-2 form (SCR, mux_rate 4.5 to 7.5 Mbit/s per file, no stuffing) |
| System header | 15 bytes, first pack only | declares one video stream and `audio_bound 0`; the audio is not declared |
| PES packets | 3 × 4096 + 1 × 4082 bytes per pack | length fields 4090 and 4076; the first pack's packets shrink by the system header; the last pack ends with short packets and 0 to 3 padding streams |
| PES header | header_data_length 10 (PTS + 5 stuffing bytes); 13 (PTS + DTS) in the first video packet | first PTS 0.042 to 0.049 s |
| Streams | video stream id E0; audio in private stream 1 (BD) with sub-stream id FF | nothing else; audio packets are 28 to 43 % of all packets, interleaved in PTS order |
| Trailer | 4 bytes | MPEG program end code |

**Video** (identical in all 38 files; `survey_all.txt`, `ffprobe_904_A1.txt`, `decode_all.txt`)

- MPEG-2 video, **Main profile, 512 × 448, progressive, exactly 30 fps** (frame_rate_code 5, not 29.97),
  aspect_ratio_information 1 (square samples; ffprobe reports DAR 8:7, which must be ignored for display),
  sequence bit_rate field 4 Mbit/s, vbv_buffer_size 80 (1.3 MB), BT.470BG colour, GOPs of 18 frames with two
  B-frames between references (about 10 I, 50 P, 120 B per 180 frames).
- ffmpeg decodes all 38 files with **zero warnings**; 91,322 frames in total (50.7 minutes, 25.4 unique because
  every `_W` twin has the same frame count), at well over 1000 frames/s in software including the XOR and the 7z
  stream. Frame counts: 904_A1 to A4 180 (6.0 s), 904_B1 to B4 220 (7.3 s), END 370 (12.3 s), TAKA 750 (25.0 s),
  L5_CI 270 (9.0 s), 1905 1010 (33.7 s), 602_2 1540 (51.3 s), 605 3830 (127.7 s), A06_2 1100 (36.7 s), P01 3390
  (113.0 s), TOPO 2361 (78.7 s), URA and STAFF 14720 each (490.7 s).

**Audio** (`audio_904_A1.txt`, `audio_904_B1.txt`, `audio_L5_CI_B.txt`, `survey_all.txt`)

- The private-stream-1 packets carry one **Sony ADS stream** (`SShd` header chunk of 24 bytes: format, sample rate,
  channels, interleave, loop start, loop end; then an `SSbd` body chunk) sliced across the packets. Each packet
  payload is a constant 4-byte prefix (sub-stream id FF plus 3 constant bytes, the same in every file checked)
  followed by the next slice. The carried bytes equal the `SSbd` body size exactly.
- Format field 1 = **16-bit PCM, 2 channels, interleave 512, no loop**. Samples are **little-endian int16** (lag-1
  autocorrelation 0.98 versus 0.17 read big-endian) and **block-interleaved: 512 bytes of left, then 512 bytes of
  right** (the end of a left block correlates 0.98 with the start of the next left block and 0.03 with the start of
  the following right block). No ADPCM anywhere: no audio decoder is needed.
- The sample-rate field says **48048** in 15 of the 17 files with audio and 48000 in the L5_CI pair. 48048 is
  48000 × 1.001, the 29.97-to-30 fps speed-up; at that rate the audio length matches the video to 1 ms (6.005 s
  against 180 frames). Whether the game honours the field or plays at 48 kHz (0.1 % slow, 0.5 s of drift over the
  8-minute movies) is an S6b question.
- **No audio stream in END, END_W, TAKA_N, TAKA_W** (video only).

**`_W` variants** (`sizes.txt`, `pairs_small.txt`, `letterbox.txt`, `anamorphic.txt`)

- 19 pairs; a `_W` file is within −0.4 % to +1.7 % of its partner's size, has the same sequence header, frame
  size, aspect code, frame count and audio, and first differs 257 to 5147 bytes in, inside the first picture's
  slice data; no 16 KB block is shared except in END (13/426) and L5_CI (260/521: the first 58 blocks, a logo on
  black, are byte-identical).
- Decoded frames show **no letterbox bars** in either variant (2 dark rows top, 0 bottom, 1 dark column each side).
- Geometric test on 9 frames from 8 pairs: stretching the `_W` frame horizontally by 4/3 and centre-cropping
  reproduces the base frame with correlation 0.885 to 0.996, against at most 0.84 for the direct comparison and for
  vertical or horizontal crops. So **`_W` is an anamorphic re-render: a 16:9 field of view squeezed into the same
  512 × 448 frame, meant to be displayed stretched to 16:9**; the base file is the 4:3 render displayed stretched to
  4:3. The odd names `TAKA_N`/`TAKA_W` and `L5_CI_B`/`L5_CI_W` follow the same scheme.

### What changes in the docs

- project-brief.md §6, "Movies" row: replace "Container format unknown" with the facts above (MPEG-2 program
  stream with 16 KB packs, MPEG-2 video 512 × 448 progressive 30 fps Main profile, PCM16 LE stereo in private stream
  1 as a Sony ADS stream, XORed with a 2048-byte LCG-generated table, `_W` = anamorphic 16:9 render). Keep "to be
  confirmed" on where the game generates the table until S6b.
- project-brief.md §6 "What this tells us" item 4, §11 S6 row: S6a done; S6b remains (generator and player
  addresses).
- project-brief.md §7 Display, "True widescreen": the `_W` files are genuine anamorphic 16:9 renders of every
  movie, so the FMV half of widescreen is solved by selecting `_W`; only the in-game 3D and HUD still need S7's
  answer. Their existence does not by itself prove the game has a 16:9 display mode.
- project-brief.md §7 Audio and video, "Full-motion video playback": no longer depends on identifying the format;
  depends on S6b (table source, player interface).
- project-brief.md §12 R6: movie half resolved; residual risk is the table source (constants in the ELF) and the
  player's sceMpeg/IPU interface.
- architecture.md §7.2: the demux is a 2048-byte periodic XOR plus a trivial program-stream parser (16 KB packs,
  E0 and BD/FF); audio needs no decoder (PCM16 LE, 512-byte channel blocks, 48048 Hz field); `IMovieDecoder` only
  needs MPEG-2 Main profile 512 × 448; presentation must apply the 4:3 or 16:9 stretch by file variant, never the
  MPEG aspect code; the table comes from the region profile or from setup-time recovery.
- architecture.md §14: add the movie table and its generator constants to the game-derived values that never enter
  the repository or tool output.
- tech-stack.md §5, "Movie decoding": see the decision below.
- `data/regions/` NTSC-U profile: movie entries (generator kind, constant addresses after S6b, the 19 variant pairs).
- `Docs/formats/mvi.md` was not written in this session (only this file could change); the tables above are ready
  to lift into it.

### Decision impact

- tech-stack.md §5 movie decoder: **recommend FFmpeg libavcodec, LGPL build, dynamically linked, with only the
  `mpeg2video` decoder enabled**; no libavformat (our demux is a few hundred lines) and no audio codec. Main-profile
  512 × 448 at 30 fps decodes at well over 1000 fps in software here, libavcodec is maintained, and an LGPL shared
  library keeps the host clean. libmpeg2 (GPL-2.0-or-later, compatible) stays the lighter fallback if FFmpeg's
  build or packaging cost is judged too high; pl_mpeg remains unsuitable (MPEG-1 only). The IPU's small deviations
  from the MPEG-2 reference decoder (IDCT precision) are not worth reproducing for FMV.
- No ADR yet. Propose one after S6b: "movie table from region-profile constants" versus "setup-time recovery from
  the user's disc" (the second works today without any ELF knowledge).
- D3 (game module built locally): unchanged; the table is data, not code, and belongs to the region profile or
  setup, not to the recompiled module.

### Local artifacts (git-ignored)

- `work/mvi/inventory.txt`, `sizes.txt`: file list, sizes, pair deltas.
- `scan_904_A1.txt`, `scan_904_B1.txt`: entropy per 64 KB window, start-code counts, lag equality, block-position
  variability (brief steps 1 and 3).
- `period_904_A1.txt`, `trykey_904_A1.txt`, `key_recovery.txt`, `key_refine.txt`, `key_final_verify.txt`: the road
  to the table. The table itself is **not stored anywhere under the repository tree**; `scripts/mvi_key3.py`
  regenerates it into the scratchpad from the disc.
- `pairs_small.txt`: first differing offsets and identical-block counts for 7 pairs and 3 unrelated files (step 2).
- `pt_stats_904_A1.txt`, `ffprobe_904_A1.txt`, `ffmpeg_null_904_A1.txt`, `survey_all.txt`, `decode_all.txt`:
  step 4 for one file and for all 38.
- `audio_904_A1.txt`, `audio_904_B1.txt`, `audio_L5_CI_B.txt`: ADS framing, endianness, interleave, PTS ranges,
  system header.
- `letterbox.txt`, `anamorphic.txt`: `_W` geometry.
- `find_table.txt`, `find_table_disc.txt`, `iso_listing.txt`: table searches over the whole disc.
- `table_structure.txt`, `lcg_fit.txt`, `lcg_known.txt`: generator identification.
- `scripts/`: all scripts used (first-party, GPL-3.0-or-later). Pipeline to reproduce:
  `7z e -so "$DQ8_ISO" MOVIE/E904/904_A1.MVI | python3 -I scripts/mvi_decrypt.py <table> | ffprobe -f mpeg -i pipe:0`.

### Open questions

1. **Table generator (S6b):** find the routine that fills a 0x800-byte buffer with 512 words of
   `x = a·x (+ c) mod 2^32` from a constant seed, and the XOR loop that applies it per 2048-byte sector of movie
   data (expected in the CD read path or the sceMpeg data-supply callback, since the period is the sector size).
   Record the addresses of the routine, the seed and the multiplier, never their values, and whether the code uses
   the `c = 0` form or the equivalent `c = 2^31` form. Check whether the same generator or table touches anything
   else (S4 should watch for it in `DATA.DAT` reads). → S6b
2. **Table scope (S6b):** confirm the seed is a constant (one table for every file, as observed) and not derived
   from the file name, size or position, and that the XOR runs before sceMpeg sees the data. → S6b
3. **Audio path (S6b):** where the BD/FF payloads go (sceMpeg private-stream callback → IOP → SPU2 transfer, or
   `PCMPLAY.IRX`), whether the game parses the `SShd` header and honours 48048 Hz (pitch or resampling) or
   hard-codes 48 kHz stereo with 512-byte channel blocks, and how A/V sync is kept. → S6b, feeds architecture §7.1
4. **Presentation (S6b, S7):** IPU output format used (RGB32 with colour-space conversion, or indexed), the sprite
   size and placement that draws 512 × 448 into the frame buffer in 4:3 and 16:9 modes, interlacing, and the
   setting or condition that selects a `_W` file. → S6b for the code path, S7 for the visible display option
5. The meaning of the 3 constant bytes after the FF sub-stream id, and whether the player relies on the padding
   streams or the system header. → S6b
6. Whether the IPU's deviations from the MPEG-2 reference decoder are visible in these movies. → M4, visual check

### S6b (Ghidra, static analysis of the boot ELF)

**Session:** 2026-10-19, about 45 minutes. **Stop reason:** done. Project `work/ghidra/s6` (a copy of
`work/ghidra/base`), queried read-only with `tools/ghidra/MovieXrefs.java` (new, committable): anchors are the
generator constants (scratch file, never committed), the SDK and game strings, the IPU register and DMA ranges, and
explicit addresses; five runs of 5 to 20 seconds each (`work/ghidra/s6-run[1-5].log`). Decompiled output lives in
`work/ghidra/s6-out[1-5]/` (git-ignored, game-derived). Every address below is in the main image unless said
otherwise; the ELF's `gp` is `0x003D9770`, so the decompiler's `*Gpffffxxxx` globals are `gp` plus that signed offset.

**Generator and XOR: `FUN_0019F130` (444 bytes) is the movie read routine, and the whole transform lives in it.**
- Signature `(ctx, buffer, size)`. It rounds `size` down to whole 2048-byte sectors, reads that many bytes from the
  open movie file through the game's file API (`FUN_0024FEB0`), then XORs every sector in place and returns the byte
  count. Callers: the player's open routine `FUN_001A1C60` (first fill) and the reader thread `FUN_001A0B40`
  (steady state, 16 KB per call into an 80 KB ring at `gp-0x6D70`, i.e. `0x003D2A00`).
- Seed material is an **obfuscated 23-byte string in the image at `0x00395790`** (file offset `0x295910`), decoded
  on the stack with an 8-bit congruential byte generator (two immediates in the loop at `0x0019F15C`–`0x0019F17C`).
  The decoded string is folded into a 32-bit seed by summing its bytes into four lanes (byte `i` into lane `i mod 4`,
  little-endian). The 32-bit multiplier is built by the lui/ori pair ending at `0x0019F248`, and the sector loop
  does `word ^= x; x *= multiplier` for 512 words. There is no increment (the `c = 0` form from S6a) and the seed is
  recomputed from the string for every sector, which is why every sector of every file uses the same table. Values
  of the string, the two byte-generator immediates, the multiplier and the seed stay out of the repository.
- **Verified:** reproducing this recipe in Python from the ELF bytes regenerates the S6a table exactly (2048/2048
  bytes, seed equal to the first word) — `work/mvi/recipe_verify.txt`, `work/mvi/scripts/verify_recipe.py`.
- The XOR is applied **before** sceMpeg sees anything: the read routine fills the demux ring that
  `sceMpegDemuxPssRing` consumes, and the SDK library never touches the file. The flag `gp-0x6D30` enables the XOR
  and is set unconditionally when the file is opened (`FUN_0019F2F0`); the table is used for nothing else that was
  found. Because reads are rounded down to whole sectors, the trailing 4-byte end code of the file is never read.
- Consequence for the port: **the recompiled game performs the XOR itself.** The runtime never needs the table or
  its constants for playback; it only has to serve the raw `.MVI` bytes to the game's file reads and HLE the sceMpeg
  or IPU layer, which then receives a plain MPEG-2 program stream. The S6a recovery method stays useful for tools and
  tests.

**File name and the `_W` choice.**
- `FUN_0019F2F0` (692 bytes, called from the open routine) builds the path: `cdrom0:\MOVIE\` + the upper-cased name,
  then replaces every `.PSS` in it by `.MVI` (the data pipeline used PSS names; the disc carries MVI). A name with a
  drive prefix is treated as a host path for development builds. It also records the file size.
- **Two entry points play movies and both apply the same rule:** `FUN_001CE350` (316 bytes; an event-script command
  handler reached through the pointer table at `0x00395E80`, 8-byte entries, no direct callers) takes a name from the
  table at `0x00411A80` and, if the screen-size getter returns 1 and the name does not already end in `_W.PSS`,
  rewrites `.PSS` to `_W.PSS`. `FUN_0034A160` (1752 bytes, called from the 12,760-byte sequencer `FUN_0017CDD0`)
  formats a name, appends the widescreen suffix on the same condition and then `.PSS`. Both then call the movie
  manager `FUN_001CE490`.
- **The selector is one byte at `0x003D275C` (`gp-0x7014`)**: getter `FUN_00168FE0`, a second reader
  `FUN_00168F90`, setter `FUN_00168FF0`, whose only caller `FUN_0036CFD0` copies it from the first word of the
  game's saved system-settings block (callers `FUN_0017FFB0`, `FUN_00309E30`, `FUN_0036D100`: settings load and the
  settings screen). This is the "Screen Size: Normal 4:3 / Wide Screen 16:9" option S7 saw on first boot. The same
  byte reaches the 3D camera: `FUN_00214E20` passes it as the last argument of the projection set-up `FUN_002167A0`,
  so the game has a native 16:9 rendering mode for 3D as well (pointer for the widescreen feature; not analysed).

**Player structure (all game code).**
- Manager `FUN_001CE490` (1468 bytes): allocates a `moviework` GS buffer of screen size and format, an 0x17980-byte
  work area, optionally loads a caption resource for the movie (overlay drawn per frame by `FUN_001B1F00`), opens the
  movie through `FUN_001A2450` → `FUN_001A1C60`, starts the threads (`FUN_001A1A00`), waits for the start flag, then
  runs one loop iteration per game frame: pad update, caller callbacks, `FUN_001A1720` (draw), `FUN_001A02C0`
  (frame hand-over), captions, present; stops with `FUN_001A13D0` when the ring drains or the audio state ends.
- Open routine `FUN_001A1C60` (1920 bytes): volume mode from a name table at `0x003957D0`, buffers (sceMpeg work
  0x1C000 + the SDK's own 0x19A0 minimum, video ES staging 0x8000 = 16 slots of 2 KB with a 0x100-byte DMA tag
  chain, demux ring 0x14000, two decode buffers of 0xE0000 = 512×448×4 each, audio ring 0xC000 with a 0x6000 start
  threshold), DMAC enable, IPU init, `sceMpegCreate`, five general callbacks and two stream callbacks (SDK stream
  type 0 = video at `FUN_001A06C0`, type 2 = PCM at `FUN_001A0470`; type 2 is private stream 1 with sub-id FF, as
  the demux table at `0x00390E40` and the fallback in `sceMpegDemuxPssRing` confirm), first fill of the ring.
- Threads (`FUN_001A1A00`): a 2 KB-stack helper `FUN_001A1050`, the decoder `FUN_001A0FE0` (16 KB stack) and the
  reader `FUN_001A0B40` (16 KB stack), plus an INTC handler (`FUN_001A0460`, interrupt 2 = vblank start) and a DMAC
  handler (`FUN_001A02B0`, channel 2 = GIF).
- Reader thread `FUN_001A0B40` (1180 bytes): refills the ring 16 KB at a time, feeds audio to the IOP, calls
  `sceMpegDemuxPssRing` (`FUN_001155C0`); **playback starts** when both decode slots are full and the audio preload
  threshold is reached: it waits for the GS FIELD bit to read 0 (`FUN_00109220`), issues the sound-driver start
  command with the IOP ring address and size, marks the audio state as playing and unmutes.
- Decoder thread `FUN_001A0FE0` → `FUN_0019E840` (556 bytes): `while (!sceMpegIsEnd) { wait for a free slot;
  sceMpegGetPicture(mp, slot, 896 macroblocks); on the first picture build the two GIF upload packets of each slot;
  mark the slot filled }`.

**SDK library functions identified (addresses for the HLE map and for M4; names are the SDK names by behaviour).**

| Address | Role (probable SDK name) | Evidence |
|---|---|---|
| `0x00110728` | `sceMpegCreate` | work buffer minimum 0x19A0, internal 8-slot 0x800 ring, callback table |
| `0x00111540` | `sceMpegAddCallback` | type, function, data |
| `0x001159C0` | `sceMpegAddStrCallback` | stream-type table at `0x00390E40`, 64-entry callback list |
| `0x001155C0` | `sceMpegDemuxPssRing` | pack and end-code parsing, PES dispatch, private-stream fallback |
| `0x001113C8` → `0x00110B28` | `sceMpegGetPicture` | dest pointer (uncached), macroblock count; the SDK strings about aborted picture fetches live here |
| `0x001114E8` | `sceMpegIsEnd` | returns the work area's end flag |
| `0x00111468` | `sceMpegReset` | re-initialises state and pulses an IPU_CTRL bit |
| `0x001112D0` → `0x001161B0` | IPU init (libipu) | IPU reset, SETIQ intra and non-intra from the matrices at `0x00390EF0`, SETVQ from `0x00390F40`, SETTH, reset again; 30 references into the IPU register page |
| `0x001208E0` / `0x00120930` | DI / EI wrappers | not IPU calls, despite being called around every DMA change |
| `0x0011A588` / `0x0011A768` / `0x0011A2C0` | `sceSifBindRpc` / `sceSifCallRpc` / RPC init | the three `SceSifrpc*` semaphore names |

**IPU data path (game side).**
- Video ES bytes go to the IPU through **DMA channel 4 (to-IPU, `0x1000B400`)** as a circular 16-slot chain of
  2 KB REF tags over the 0x8000 staging buffer (`FUN_001A0150` builds the chain and starts the channel; the
  brief's channel assignment is reversed: `0x1000B000` is channel 3, from-IPU). The video stream callback copies
  each PES payload into the staging ring and records (PTS, DTS, offset, length) per packet in a 512-entry table
  guarded by a semaphore; the SDK's stop-DMA, restart-DMA and error callbacks (`FUN_0019FD80`, `FUN_0019FC60`,
  `FUN_0019F8B0`) re-aim the two IPU channels and save or restore IPU_CTRL and IPU_BP around picture boundaries;
  the timestamp callback `FUN_001A0A50` → `FUN_0019F5B0` maps the to-IPU channel's current address and IPU_BP back
  to the packet whose timestamps are returned to the library.
- **IPU output format:** RGBA32 in macroblock order, 1024 bytes per 16×16 block, delivered by the library through
  channel 3 into the 0xE0000-byte decode slot (`sceMpegGetPicture` with the uncached destination); no RGB16 and no
  indexed mode is used.

**Presentation (what the GS sees).**
- `FUN_0019E5F0` (588 bytes) prebuilds, per decode slot, two GIF packets that upload the frame into the `moviework`
  GS buffer (`[gp-0x6D74] + 0x38` holds its base and width) as 896 IMAGE transfers of one 16×16 RGBA32 tile each
  (BITBLTBUF, TRXPOS at the tile's pixel position, TRXREG 16×16, TRXDIR host-to-local, one 64-quadword IMAGE tag per
  tile), ending with TEXFLUSH. `FUN_001A02C0` kicks both packets of the oldest filled slot through the GIF queue in
  one manager iteration and frees the slot, counting an underrun when no slot is ready. Why two packets per slot was
  not resolved (both are built with the same arguments; a double-buffered `moviework` is the likely reason).
- `FUN_001A1720` (728 bytes) draws the frame every iteration: a black quad grid first (for the 512×448 area in 32×32
  steps), then the same grid textured from `moviework` at 1:1 pixel coordinates with neutral colour. **No scaling
  and no aspect handling anywhere in the player:** the movie is drawn pixel-for-pixel into the 512-wide frame
  buffer and the CRTC's 4:3 or 16:9 stretch does the rest, which is exactly why the `_W` files are anamorphic.

**Audio path and A/V sync.**
- The PCM stream callback `FUN_001A0470` (592 bytes) skips the 4-byte sub-stream prefix of each packet and the
  first 40 bytes of the stream (the ADS header, counted, **never parsed**: the 48048 Hz field is ignored) and copies
  the samples into the 0xC000-byte EE ring. Per reader-thread iteration `FUN_0019EDE0` (504 bytes) tops up a
  0x6000-byte ring in IOP memory (allocated through `sceSifAllocIopHeap`, zero-filled by SIF DMA at start): in the
  playing state it asks the sound driver for the IOP play position and sends whole 1 KB blocks by SIF DMA
  (`FUN_0019EA70`); before the start it preloads. Three sound-driver commands are used (channel set-up at open,
  volume for mute, fade and the volume modes "bgm", "se", "max", "off", and start/position/stop), all posted through
  the game's sound-command mailbox at `0x0041ADC0` (`FUN_0024D710`) to its sound thread.
- The IOP side is `PCMPLAY.IRX` (module name `pcm_mixer`, 8.6 KB): a SIF RPC server (registration at
  `.text+0x2C4` of the module), importing libsd ordinals 5, 18, 20 and 26 (init, voice transfer, voice-transfer
  status and one more by the public ps2sdk table) and no block transfer, so it feeds the SPU2 through voice
  transfers rather than auto-DMA; how it turns 16-bit PCM into something a voice can play (ADPCM encoding on the
  IOP, most likely) is M4 work. The EE-side RPC client is `FUN_002595D0` (bind, client struct `0x0041B500`, 64-byte
  argument buffer `0x0041B4C0`, own thread `FUN_002592C0`) with the call wrapper `FUN_00259360`; the hop from the
  sound thread's mailbox to this client was not traced.
- **Sync model: open loop, two free-running clocks.** Video: one decoded frame is consumed per manager iteration
  (the game loop; 30 Hz per S7), decoder and reader threads simply keep the two slots and the rings full; PTS and
  DTS are tracked only for the library. Audio: the IOP ring is kept full against the IOP's play position; the SPU2
  clock paces it. Nothing compares the two, so the 0.1 % rate mismatch found in S6a drifts up to about half a
  second over the two 8-minute movies in the original game as well. End of movie: the player finishes when the demux
  ring drains below 5 bytes or the audio state reaches "ended" (`FUN_001A1390`); the stop routine also issues a
  stop command and frees the IOP buffers.

**What changes in the docs**
- `Docs/formats/mvi.md`: add the generator recipe in prose (obfuscated string at `0x00395790`, lane sum, 32-bit
  multiplicative generator, per-sector reseed), the read-routine address, the sector rounding, the `_W` selector
  byte and the two entry points, the IPU/GS presentation facts, and the audio path; keep all values out.
- architecture.md §7.2: replace "HLE the IPU interface" with the two concrete options this spike enables: (a) HLE
  at the **sceMpeg API** (the nine library entry points above, one `IMovieDecoder` behind them, raw file bytes
  served to the recompiled reader, which does the XOR itself; the player's own threads, GS upload and audio feed stay
  as they are) or (b) HLE at the **IPU and DMAC level** (channels 3 and 4, the IPU command set, macroblock-order
  RGBA32 output, EE-side motion compensation runs recompiled). (a) is far smaller and uses libavcodec directly;
  (b) is what emulators do. Recommend (a); record the choice as an ADR when the runtime design is accepted.
- architecture.md §7.1: movie audio arrives as PCM blocks pushed to `PCMPLAY.IRX` over SIF RPC and DMA; the
  sound-stack HLE must offer that server (position query, start, stop, volume) and mix 48 kHz PCM.
- architecture.md §5.1 / §14: the table is produced by game code at run time from data in the ELF; no table or
  constant goes into the region profile for playback. Only tools that decrypt movies outside the game need the S6a
  recovery (disc-derived, scratch only).
- project-brief.md §6 "Movies" row: drop "to be confirmed" on the generator; §7 "True widescreen": the screen-size
  byte at `0x003D275C` also drives the 3D projection, so native 16:9 exists for gameplay too (S7 to confirm on
  screen); §11 S6 row: done.
- tech-stack.md §5 movie row: unchanged recommendation (libavcodec `mpeg2video`); add that no demux library and no
  audio codec are needed and that the decoder is fed by an sceMpeg-level HLE.
- The brief's S6b paragraph: swap the IPU DMA channel directions (`0x1000B000` = channel 3 from-IPU, `0x1000B400`
  = channel 4 to-IPU).

**Decision impact**
- tech-stack §5: confirmed; the HLE boundary (sceMpeg API) is the new proposal for architecture §7.2 and needs an
  ADR once accepted. D3 unchanged.
- Widescreen (brief §7): the FMV side is closed (select `_W` by the same byte the game uses); the 3D side has a
  native switch to study rather than a patch to write.

**Local artifacts (git-ignored)**
- `work/ghidra/s6/`: project copy. `work/ghidra/s6-run[1-5].log`, `work/ghidra/s6-out[1-5]/*.c` and `index.txt`:
  decompiled functions with callers, callees and the anchor reasons (game-derived; never commit).
- `work/mvi/recipe_verify.txt`, `work/mvi/scripts/verify_recipe.py`, `work/mvi/scripts/irx_imports.py`,
  `work/mvi/gp.txt`: recipe check, IRX import inventory, gp-relative address arithmetic.
- `tools/ghidra/MovieXrefs.java` (tracked): anchors by constants file, strings, address ranges, gp offsets and
  explicit addresses; dumps callers, callees and decompiled C per function. Usage is in its header.

**Open questions**
- Why two GIF packets per decode slot, and whether `moviework` is double-buffered (`FUN_00160450` is called
  between building them) → M4, when the GS upload is reproduced.
- The sound-thread dispatch from the mailbox commands to the PCM RPC client, the RPC protocol of `PCMPLAY.IRX`
  (position query format, start arguments) and how the module plays 16-bit PCM through voice transfers → M4 audio
  HLE (`PCMPLAY.IRX` is 8.6 KB; disassemble it with the IOP target).
- The reader `FUN_00168F90` of the screen-size byte and every other consumer of it (menus, HUD, projection
  `FUN_002167A0`) → S7 follow-up for the widescreen feature.
- Whether the SDK's motion compensation on the EE needs anything from the runtime beyond ordinary recompiled code
  when the sceMpeg API is HLE'd (it does not run at all in option (a)) → M4.
