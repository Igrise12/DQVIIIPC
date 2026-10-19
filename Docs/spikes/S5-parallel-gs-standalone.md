# S5: Does paraLLEl-GS build here and replay our GS dumps?

**Time box:** 2 sessions (S5a now, S5b after S7). **Depends on:** S5b needs S7's dumps. **Answers:** brief Q5,
risk R3, decision D5 (ADR-0004).

## Why

paraLLEl-GS (LGPL-3.0-or-later, Vulkan compute) is the proposed first GS backend (architecture.md §6.2).
It documents no embedding API, needs specific Vulkan features, and its dump reader accepts raw (uncompressed)
PCSX2 dumps of state version ≥ 9 only. This spike proves the build, the GPU, the API surface and the parity
with PCSX2 on real frames from this game.

## S5a (independent)

```bash
git clone https://github.com/Arntzen-Software/parallel-gs work/parallel-gs && (cd work/parallel-gs && ./checkout_submodules.sh)
# Tools (replayer, stream) need PARALLEL_GS_STANDALONE=OFF (Granite platform "SDL"); the library-only build is ON.
for cc in gcc clang; do case $cc in gcc) CXX=g++;; clang) CXX=clang++;; esac
  CC=$cc CXX=$CXX cmake -S work/parallel-gs -B work/parallel-gs/build-$cc -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1 | tee work/parallel-gs/configure-$cc.log
  ninja -C work/parallel-gs/build-$cc 2>&1 | tee work/parallel-gs/build-$cc.log; done
vulkaninfo --summary > work/parallel-gs/vulkaninfo.txt
```
Record: targets built (`parallel-gs`, `parallel-gs-dump`, `parallel-gs-replayer`, `parallel-gs-stream`,
`parallel-gs-repro`), Granite's extra requirements (SDL, shader toolchain), compiler errors with the new
GCC/Clang and any local patches (kept in `work/parallel-gs/patches/`). Check the RTX 4060 against the
documented feature list: `descriptorIndexing`, `timelineSemaphore`, `storageBuffer8BitAccess`,
`storageBuffer16BitAccess`, `shaderInt16`, `scalarBlockLayout`, full subgroup support, subgroup size control
with full groups (16–64). Read `gs/gs_interface.hpp` and `gs/gs_renderer.hpp` and write down the embedding
surface: device creation (Granite `Vulkan::Context`/`Device`), register writes, GIF transfers, privileged
registers, vsync/scanout, local-to-host readback, threading expectations. If the replayer only targets
RenderDoc, add a small local `--dump-png` option (candidate for `patches/parallel-gs/`).

## S5b (after S7)

Replay `work/pcsx2/dumps/*.gs` (raw, `GSDumpCompression=0`) at 1x and 2x supersampling; write frames to
`work/parallel-gs/frames/`; compare with PCSX2's own software-renderer frame of the same dump (S7 screenshot,
or `pcsx2-qt -batch -nogui <dump>` with the Screenshot hotkey) using `ffmpeg -filter_complex ssim`. Record
per-frame replay time and the SSIM per scene (title, field, menu, battle), and explain any systematic
difference (deinterlacing, field blending, AA1).

## Deliverables (findings)

Build facts per compiler, feature-check result, API surface with an estimate of the wrapper size, the LGPL
arrangement (because `parallel-gs` is a static library, our backend must be a separately built shared library
that contains the parallel-gs and Granite objects so users can relink it), performance and parity numbers, and
a D5 recommendation. Update tech-stack.md §4.2 and §5 and architecture.md §6.2 accordingly.

## Must not commit

Dumps, frames, screenshots, the clone.
