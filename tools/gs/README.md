# tools/gs/

`dq8-gs-replay`: replays a GS packet stream through our paraLLEl-GS backend, the same `IGsBackend` and
`dq8-gs-parallel` C ABI the runtime uses (`src/runtime/gs/gs_parallel_backend.h`), and writes the scanout of each vsync
to PNG or shows it in an SDL3 window. Built with `DQ8_BUILD_GS_PARALLEL` (the window also needs `DQ8_BUILD_WINDOW`);
contributor tool, not installed. Needs a Vulkan 1.3 GPU with paraLLEl-GS's features (tech-stack.md §4.2).

```bash
build/dev-clang/tools/gs/dq8-gs-replay work/pcsx2/<capture>/<scene>.gs --png work/gs-replay/<scene>
build/dev-clang/tools/gs/dq8-gs-replay work/pcsx2/<capture>/<scene>.gs --window --loop --rate 30
build/dev-clang/tools/gs/dq8-gs-replay <trace> --raw --png out --vsyncs 100-110 --ssaa 4
build/dev-clang/tools/gs/dq8-gs-replay --help
```

Inputs and outputs from the game are game-derived: keep them under `work/` (git-ignored), never in the repository.

## Inputs

- **PCSX2 GS dump**, uncompressed (`GSDumpCompression` off), state version 8 or 9, detected by its first word
  (`0xFFFFFFFF`). The GS state it starts from is restored through GIF packets only, so the replay exercises the
  runtime's own path (`gs_state_restore.h`): GS memory as one host-to-local IMAGE transfer of a 1024x1024 PSMCT32
  rectangle (the 4 MiB rearranged with the GS's PSMCT32 page, block and column layout), then the general registers by
  A+D. TRXDIR is not written and TEX0's CLUT-load field is cleared; the GIF path state and the CLUT buffer are not
  restored (the tool warns when a path was mid-packet).
- **Raw trace** (`--raw`): the same packets without a header, as our trace backend writes them and
  `parallel-gs-replayer --raw` reads them.

Packets: 0 = transfer `{u8 path, u32 size, data}`, 1 = vsync `{u8 field}`, 2 = read FIFO `{u32 size}`,
3 = privileged registers `{8,192 bytes}`. The path byte follows PCSX2: 0, 1, 2 = PATH1, PATH2, PATH3, and 3 = PCSX2's
merged GIF stream, which the tool sends through PATH3. The privileged page is handed to the backend at each vsync.

## Outputs

- `--png DIR`: `DIR/vsync_NNNNN.png` for each vsync (all, or those `--vsyncs` lists) that displays a picture, RGBA
  with opaque alpha, at the scanout size (512x448 for this game at 1x).
- `--window`: frames paced at `--rate` vsyncs per second, aspect-fitted (`--aspect 4:3|16:9`) with bilinear scaling;
  `--loop` replays until the window is closed, `--hold` keeps the last frame up, `--window-capture PATH` writes what
  the window shows after the last frame.
- A summary: vsyncs, frames read back, GIF transfers and bytes, wall time and milliseconds per vsync spent in the
  backend (GIF decode, flushes, scanout and readback).

Note on numbering: paraLLEl-GS's own replayer merges each empty vsync of a 30 Hz pair into the next frame, so its
"frame N" is not our "vsync N". For the S7 settings dump its frame 1 (S5b's parity number) is our vsync 2.

## Parity check (S5b method)

Scale our frame bilinearly to 640x480 and compare it with the screenshot embedded in the dump (PCSX2's software
renderer) using `ffmpeg -i ours.png -i ref.png -filter_complex "[0:v]format=rgb24,scale=640:480:flags=bilinear[a];[1:v]format=rgb24[b];[a][b]ssim" -f null -`.
In Debug builds Granite (`VULKAN_DEBUG`) logs each pipeline compile on stderr; Release builds do not.
