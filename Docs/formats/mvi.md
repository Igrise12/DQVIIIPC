# MVI movie container

| | |
|---|---|
| **Status** | Container, video and audio layout established by spike S6a (2026-10-19). Where the game generates the XOR table, and the player's interface, are spike S6b's questions. |
| **Source** | `Docs/spikes/S6-mvi-movie-format.md` (Findings), evidence under `work/mvi/` (git-ignored) |
| **Rule** | This page describes layout only. The XOR table, its generator constants and any movie bytes are game-derived and never appear in the repository or in tool output (architecture.md §14). |

## Files

38 `MOVIE/**/*.MVI` files, 19 pairs `X.MVI` / `X_W.MVI` (also `TAKA_N`/`TAKA_W`, `L5_CI_B`/`L5_CI_W`). Every
file is `4 + N × 16384` bytes (N from 213 to 15,219). Four files (`END`, `END_W`, `TAKA_N`, `TAKA_W`) carry no audio.
Frame counts per pair: 904_A1–A4 180, 904_B1–B4 220, END 370, TAKA 750, L5_CI 270, 1905 1,010, 602_2 1,540,
605 3,830, A06_2 1,100, P01 3,390, TOPO 2,361, URA and STAFF 14,720 each; 91,322 frames in all (50.7 minutes).

## Obfuscation layer

The whole file is XORed, from offset 0, with a **2,048-byte table that repeats every 2,048 bytes** (one CD-ROM
sector). One table serves all 38 files. Read as 512 little-endian 32-bit words the table is the output sequence of
a 32-bit linear congruential generator (`x[n+1] = a·x[n] + c mod 2^32`, with an equivalent `c = 0` form); the first
word is the seed. The table does not occur anywhere on the disc, so the game builds it at run time from constants.

How the runtime obtains it (decision pending S6b): read the constants from the user's ELF through the region profile
(addresses recorded by S6b), or recover the table at setup time from the user's own disc with the most-repeated-sector
method (a constant plaintext sector of digital silence appears 500+ times across the movies).

## Program stream (after the XOR)

Standard MPEG-2 program stream (ISO/IEC 13818-1).

| Field | Size | Notes |
|---|---|---|
| Pack header | 14 bytes at every 16,384-byte boundary | MPEG-2 form; SCR; mux_rate 4.5–7.5 Mbit/s per file; no stuffing |
| System header | 15 bytes, first pack only | declares one video stream; `audio_bound 0` (the audio is not declared) |
| PES packets | 3 × 4,096 + 1 × 4,082 bytes per pack | PES_packet_length 4,090 and 4,076; the first pack's packets shrink by the system header; the last pack ends with short packets and 0–3 padding streams |
| PES header | `header_data_length` 10 (PTS + 5 stuffing bytes); 13 (PTS + DTS) in the first video packet | first PTS 0.042–0.049 s |
| Streams | video `0xE0`; audio in private stream 1 (`0xBD`) with sub-stream id `0xFF` | nothing else; audio packets are 28–43 % of all packets, interleaved in PTS order |
| Trailer | 4 bytes | MPEG program end code |

## Video

MPEG-2 video, Main profile, **512 × 448, progressive, exactly 30 fps** (`frame_rate_code` 5, not 29.97),
`aspect_ratio_information` 1 (square samples: the derived 8:7 display ratio must be ignored), sequence bit rate
4 Mbit/s, `vbv_buffer_size` 80, BT.470BG colour, 18-frame GOPs with two B-frames between references. FFmpeg decodes
all 38 files without warnings.

## Audio

The private-stream-1 payloads carry one **Sony ADS stream** sliced across packets: a 24-byte `SShd` header chunk
(format, sample rate, channels, interleave, loop start, loop end) followed by an `SSbd` body chunk. Each PES payload
starts with a constant 4-byte prefix (`0xFF` sub-stream id plus three constant bytes) and then the next slice.

Format: **16-bit PCM, little-endian, 2 channels, block-interleaved as 512 bytes of left then 512 bytes of right**,
no loop. The sample-rate field is 48,048 Hz in 15 of the 17 files with audio (48,000 × 1.001, matching the video to
1 ms) and 48,000 Hz in the `L5_CI` pair. No ADPCM anywhere, so no audio decoder is needed. Whether the game honours
the 48,048 Hz field is an S6b question.

## `_W` variants

A `_W` file has the same sequence header, frame size, aspect code, frame count and audio as its partner and differs
inside the first picture's slice data. Neither variant is letterboxed. Stretching the `_W` frame horizontally by
4/3 and centre-cropping reproduces the base frame (correlation 0.885–0.996), so **`_W` is an anamorphic 16:9 render
squeezed into 512 × 448**, to be displayed stretched to 16:9; the base file is the 4:3 render.

## Consequences for the port

- Demux: a 2,048-byte periodic XOR plus a small program-stream parser (16 KiB packs, `0xE0` and `0xBD/0xFF`).
- Decoder: MPEG-2 Main profile only; recommendation (tech-stack.md §5) is FFmpeg libavcodec, LGPL build, dynamically
  linked, `mpeg2video` only; libmpeg2 is the fallback. No libavformat, no audio codec.
- Presentation applies the 4:3 or 16:9 stretch by file variant, never the MPEG aspect code (architecture.md §7.2).
- Region profile: movie entries (variant pairs, table source) are data, not code.

## Open (S6b)

Generator routine and its constants' addresses; whether the XOR runs before `sceMpeg` sees the data; where the
PCM goes (IOP/SPU2 path, `PCMPLAY.IRX`?) and how A/V sync is kept; IPU output format and the sprite that draws
the 512 × 448 frame; what selects a `_W` file.
