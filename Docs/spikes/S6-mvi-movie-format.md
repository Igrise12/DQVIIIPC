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
