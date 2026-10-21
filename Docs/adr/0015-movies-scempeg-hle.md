# ADR-0015: Movies through an sceMpeg HLE with a dynamically loaded libavcodec decoder

| | |
|---|---|
| **Status** | **Accepted** 2026-10-21 by the project owner (M4 planning) |
| **Related** | architecture.md §7.2; tech-stack.md §5, §10; spikes S6a and S6b; Docs/formats/mvi.md; Docs/milestones/M4-playable.md |

## Context

M3 skipped movies (the player API returns "finished" at once). Spike S6b left two ways to play them: replace the
game's `sceMpeg` library calls with a host decoder, or model the IPU and its DMA channels 3 and 4 under a decoder. The
game decrypts the `.MVI` stream itself (its own XOR in its read routine), demuxes with the SDK's ring code, and
feeds movie audio to PCMPLAY; only the picture decode touches the IPU.

## Decision

- The movie HLE replaces only what touches the IPU: `sceMpegGetPicture`, `sceMpegIsEnd`, and the game's type-0 video
  stream callback (which would feed the IPU), with hooks on Create, Delete, AddCallback, AddStrCallback, DemuxPssRing,
  Reset and the player's open. The reader thread, the XOR read, the demux ring, the audio feed, the slot ring and the
  GIF upload stay recompiled game code. The entry addresses are region-profile data (`[movie.sce_mpeg]`).
- The player's own IPU init and reset run against an IPU register stub; a decode command reaching it stops the guest.
- An elementary-stream model in the runtime alone decides how many pictures there are and when each is due; the
  decoder only paints pixels. Decoding is synchronous on the machine thread at a fixed virtual cost per picture.
- The decoder is **FFmpeg libavcodec** (`mpeg2video` only, plus libavutil; one thread, bit-exact flags, a fixed
  IDCT), wrapped in a separate shared library `dq8-movie` that dq8-game loads at run time through a small C ABI
  (`include/dq8/movie/movie_decoder_abi.h`), as `dq8-gs-parallel` is loaded. M4 uses the system libavcodec on Linux;
  a bundled LGPL build and Windows DLLs come with packaging (M5).
- `--movies auto` plays when the library loads and skips otherwise; `play` without the library fails at start-up;
  `nodecode` runs the HLE with black pictures and the same timing.

## Consequences

- The audio path of movies is the game's own (PCMPLAY, interpreted per ADR-0014).
- IPU emulation is not pursued; DMA channels 3 and 4 stay a stop if anything starts them.
- Runs without FFmpeg still work (movies skipped); the LGPL library is replaceable by the user.
- A `nodecode` run and a `play` run produce identical event logs apart from GS hashes, which keeps determinism checks
  independent of the decoder.
