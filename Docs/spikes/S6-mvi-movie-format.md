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
channels (`0x1000B000` to-IPU, `0x1000B400` from-IPU); the `sceMpeg` data-supply callback is where any
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
