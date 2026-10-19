# MVI movie container

| | |
|---|---|
| **Status** | Container, video and audio layout (spike S6a) and the game's reader, generator, player and audio path (spike S6b), both 2026-10-19. |
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

**The game builds the table itself** (S6b). The movie read routine at `0x0019F130` decodes a 23-byte obfuscated string
stored in the image at `0x00395790` with an 8-bit congruential byte generator (immediates in the loop at
`0x0019F15C`–`0x0019F17C`), folds the result into a 32-bit seed (byte *i* into lane *i mod 4*, little-endian), and for
every 2,048-byte sector runs `word ^= x; x *= multiplier` over 512 words, with the multiplier built by the `lui`/`ori`
pair ending at `0x0019F248`. The seed is recomputed per sector, which is why all sectors of all files use one table.
Reads are rounded down to whole sectors, so the file's trailing 4-byte end code is never read. Reproducing this recipe
from the ELF regenerates the S6a table exactly.

**The runtime does not need the table:** the recompiled game performs the XOR before `sceMpeg` sees the data. Only
tools that decrypt movies outside the game need the S6a recovery method (most-repeated-sector; scratch scripts under
`work/mvi/`). The values of the string, the byte-generator immediates, the multiplier and the seed never enter the
repository.

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

## Player (S6b, addresses in `SLUS_212.07`)

- **Path and variant.** `0x0019F2F0` builds `cdrom0:\MOVIE\` + the upper-cased name and rewrites `.PSS` to `.MVI`.
  Two entry points start movies (`0x001CE350`, an event-script command handler; `0x0034A160`, from the sequencer
  `0x0017CDD0`); both insert the `_W` suffix when the screen-size byte at `0x003D275C` (getter `0x00168FE0`,
  setter `0x00168FF0`, loaded from the saved system settings) is 1. The same byte is passed to the 3D projection
  set-up `0x002167A0`, so 16:9 is a native mode for gameplay as well.
- **Manager and threads.** Manager `0x001CE490`; open routine `0x001A1C60` (sceMpeg work area, 16-slot video ES
  staging ring, 0x14000-byte demux ring, two 512 × 448 × 4 decode buffers, 0xC000-byte audio ring); reader thread
  `0x001A0B40` (16 KiB per refill, demux, audio feed; starts playback when both decode slots are full and the audio
  preload threshold is reached), decoder thread `0x001A0FE0` → `0x0019E840` (`sceMpegGetPicture` per slot), vblank
  and GIF handlers.
- **SDK entry points (HLE map).** `sceMpegCreate` `0x00110728`, `AddCallback` `0x00111540`, `AddStrCallback`
  `0x001159C0` (stream table at `0x00390E40`), `DemuxPssRing` `0x001155C0`, `GetPicture` `0x001113C8` →
  `0x00110B28`, `IsEnd` `0x001114E8`, `Reset` `0x00111468`, IPU init `0x001112D0` → `0x001161B0`; SIF RPC
  bind/call `0x0011A588`/`0x0011A768`.
- **IPU path.** Video ES bytes reach the IPU through DMA channel 4 (to-IPU, `0x1000B400`) as a 16-slot chain of
  2 KiB tags; decoded pictures come back through channel 3 (from-IPU, `0x1000B000`) as RGBA32 in macroblock order
  (1,024 bytes per 16 × 16 block) into the decode slot.
- **Presentation.** Per slot, two prebuilt GIF packets upload the frame as 896 IMAGE transfers of one 16 × 16
  RGBA32 tile into the `moviework` GS buffer; the draw routine `0x001A1720` paints a black 32 × 32 quad grid and then
  the textured grid at 1:1 pixel coordinates. **No scaling or aspect handling in the player:** the CRTC's 4:3 or
  16:9 stretch does it, which is why the `_W` files are anamorphic.
- **Audio.** The PCM stream callback `0x001A0470` skips the 4-byte sub-stream prefix and the 40-byte ADS header
  (never parsed: the 48,048 Hz field is ignored) and fills the EE ring; `0x0019EDE0` tops up a 0x6000-byte IOP ring
  by SIF DMA against the IOP play position; three sound-driver commands (set-up, volume, start/position/stop) go
  through the game's sound mailbox to `PCMPLAY.IRX` (SIF RPC client `0x002595D0`). Sync is open-loop: one frame per
  game-loop iteration, audio paced by the SPU2, so the 0.1 % rate mismatch drifts in the original game too.

## Open

Why two GIF packets per decode slot (`moviework` double-buffering?); `PCMPLAY.IRX`'s RPC protocol and how it plays
16-bit PCM through voice transfers (M4 audio HLE); the other consumers of the screen-size byte (menus, HUD) for the
widescreen feature.
