# ADR-0004: GS behind an interface; paraLLEl-GS as the first backend, a trace backend early, a native renderer later

| | |
|---|---|
| **Status** | **Accepted** 2026-10-20 by the project owner (evidence: spikes S5a, S5b and S7, 2026-10-19) |
| **Related** | project-brief.md §14 (D5), architecture.md §6 and §12, tech-stack.md §4.2 and §5 |

## Context

The Graphics Synthesizer is the riskiest component. paraLLEl-GS (Arntzen-Software, LGPL-3.0-or-later) emulates it
with Vulkan compute and supports 2x–16x super-sampling. Spike S5a built commit `cc6184a` (Granite `16e7395`)
unpatched with GCC 16 and Clang 23; the RTX 4060 meets every documented and undocumented feature requirement
(`bufferDeviceAddress` and 32 KiB of compute shared memory among the undocumented ones); the embedding API is the
`GSInterface` class (about 25 methods, all calls from one thread). Spike S7 captured raw PCSX2 dumps of this game
(512×448 field-interlaced at 30 Hz, menu at 60 Hz, two CRTC circuits merged through PMODE). Spike S5b replayed them:
SSIM 0.992–0.997 against PCSX2's software renderer on field, menu, movie and settings frames; 5 ms of GPU per field
frame at 1x and 18.5 ms at 16x; a real double-resolution picture through backbuffer promotion, super-sampled textures
and high-resolution scanout; about 8 ms of CPU per field frame for GIF decode. Battle has no dump yet.

## Decision

- The runtime talks to the GS only through `IGsBackend` (architecture.md §6.1).
- **paraLLEl-GS is the first backend.** It is built into one shared library (`dq8-gs-parallel`) that contains our
  GPL wrapper, parallel-gs and the four Granite archives it needs with hidden visibility, exporting only our C ABI,
  so the LGPL relinking terms are met; its source and build scripts ship with the host.
- A **trace backend** (records packets, draws nothing) exists from the start for headless tests and replay.
- A **native renderer** comes after M6; it must handle framebuffer-as-texture, PMODE circuit merging, 16-bit targets
  with channel masks and texture/depth aliasing from day one (S7), and may skip mipmaps, dithering and readbacks.
- The GS backend runs on a dedicated GS thread (GIF decode alone is about 8 ms per field frame); the pipeline cache is
  persisted to avoid the 1.4 s first-launch stall.

## Consequences

- GPU baseline: Vulkan 1.3 with the feature list in tech-stack.md §4.2; low-end GPUs are an open question.
- Resolution scaling is available with the first backend (super-sampling and promotion), without the native renderer.
- Upstream accepts no contributions, so our changes (replayer `--dump-png`, any promotion fix) live in
  `patches/parallel-gs/`.
- Parity on the battle scene is unverified until spike S7's battle run exists.
