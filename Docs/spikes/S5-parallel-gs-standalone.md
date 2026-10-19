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

## Findings

**Session:** 2026-10-19, one session (S5a only). **Stop reason:** done for S5a; S5b blocked on S7 dumps.

Everything below was measured on the development machine of tech-stack.md §12 (GCC 16.2.1, Clang 23.1.1,
CMake 4.4.3, Ninja 1.13.2, NVIDIA GeForce RTX 4060, driver 615.71.09, Vulkan loader 1.4.357). Raw evidence lives
under `work/parallel-gs/` (git-ignored); no file there contains game data: the only dump replayed is a synthetic
one written by paraLLEl-GS's own sandbox tool.

### Facts

**Checkout and what the build needs**

- paraLLEl-GS commit `cc6184a` (2026-10-06), Granite `16e7395` (2026-08-31), volk `47cddf7` (1.4.350-4),
  Vulkan-Headers `11d6898` (v1.4.358). Licenses: paraLLEl-GS `COPYING.LGPLv3` with `LGPL-3.0+` SPDX tags on every
  file; Granite MIT; volk MIT; Vulkan-Headers Apache-2.0 or MIT; `renderdoc_app.h` MIT. The README still says
  "external contributions are currently not accepted" and "dual-licensing may be possible". — evidence:
  `git -C work/parallel-gs log -1`, `git submodule status --recursive`, the LICENSE files.
- `checkout_submodules.sh` fetches only Granite, volk and Vulkan-Headers. That is enough for
  `-DPARALLEL_GS_STANDALONE=ON` (library only: Granite platform `null`, no SPIRV-Cross, no runtime shader
  compiler, `GRANITE_SHIPPING`). The default configuration (which the brief's command line uses, and which is
  the only one that builds the tools) forces `GRANITE_RENDERER`, SPIRV-Cross, the runtime GLSL compiler and the
  SDL platform, and therefore needs ten more Granite submodules (spirv-cross, shaderc, glslang, spirv-tools,
  spirv-headers, stb, rapidjson, meshoptimizer, muFFT, fsr2) plus SDL3: either the vendored `sdl3` submodule
  or `-DGRANITE_SYSTEM_SDL=ON` against the system SDL3 3.4.16, which is what was used here. — evidence:
  `work/parallel-gs/CMakeLists.txt` lines 36-54, `Granite/third_party/CMakeLists.txt`,
  `work/parallel-gs/configure-*.log`.
- The GS shaders are precompiled: `gs/shaders/slangmosh.hpp` (2.09 MB generated header holding the SPIR-V of
  16 GLSL files, 6.6k lines of shader source) is checked in, so the library needs no glslang or shaderc at build
  or run time. Regenerating it needs Granite's `slangmosh` tool (`gs/slangmosh.sh`), only if we ever patch a
  shader. — evidence: `ls -la work/parallel-gs/gs/shaders`.
- Code size: `gs/` is 14.3k lines (gs_renderer.cpp 5,200, gs_interface.cpp 4,721, page_tracker.cpp 982,
  gs_registers.hpp 752); the dump reader and writer are 633 lines; the three tools are 1,042 lines. C++14
  (`CMAKE_CXX_STANDARD 14`), Release flags `-std=gnu++14 -DGRANITE_SHIPPING -DNDEBUG`. A consumer of
  `libparallel-gs.a` needs seven include directories: `gs`, `Granite/vulkan`, `Granite/util`, `Granite/math`,
  `Granite/application/global`, `Granite/third_party/volk`, `Granite/third_party/khronos/vulkan-headers/include`.
  — evidence: `wc -l`, `ninja -C work/parallel-gs/build-gcc-standalone -t commands gs/libparallel-gs.a`.

**Builds (zero errors, zero upstream patches needed)**

| Configuration | Compiler | Steps | Wall | Warnings | Result |
|---|---|---|---|---|---|
| library only (`PARALLEL_GS_STANDALONE=ON`) | GCC 16.2.1 | 89 | 15 s | 2 | `libparallel-gs.a` (1.28 MB, 4 objects), `libparallel-gs-dump.a`, `libanalog-video.a`, `parallel-gs-sandbox`, `analog-sandbox`, 19 Granite archives (4.4 MB in all) |
| library only | Clang 23.1.1 | 89 | 11 s | 6 | same targets; `libparallel-gs.a` 1.23 MB |
| default + `GRANITE_SYSTEM_SDL=ON` (tools) | GCC 16.2.1 | 476 | 455 s | 3 | `parallel-gs`, `parallel-gs-dump`, `parallel-gs-replayer` (14.3 MB), `parallel-gs-stream` (18.3 MB), `parallel-gs-repro`, `parallel-gs-sandbox`, `analog-video`, `analog-sandbox` |
| default + `GRANITE_SYSTEM_SDL=ON` (tools) | Clang 23.1.1 | 476 | 131 s | 12 | same eight targets; `parallel-gs-replayer` 11.2 MB |

- The warnings are all upstream and harmless: two unused functions in `sandbox/gs_test.cpp` (both compilers),
  Clang adds three `-Wshadow` for the `file` member of `dump/gs_dump_parser.hpp:49` and one unused function in
  `Granite/vulkan/command_buffer.cpp:375`; the GCC tools build adds one `-Wstringop-overflow` inside
  `stb_image.h`; the Clang tools build adds the same `-Wshadow` and unused-function set, an unused global in
  FSR 2, and a missing `override` plus a non-literal format string in `tools/gs_stream_replayer.cpp`. The Clang
  tools build overlapped with the GCC resume and the replayer runs for part of its 131 s. — evidence:
  `work/parallel-gs/build-*.log`.
- About 380 of the 476 tool-build steps are the SPIR-V toolchain (shaderc, glslang, SPIRV-Tools, SPIRV-Cross),
  which the library itself never uses. The GCC tools build stopped once at step 452 with a missing
  `stb_image_write.h`: that was my own patch landing mid-build before CMake had re-run, not an upstream error;
  the resumed build finished in 8 s (`build-gcc-resume.log`). — evidence: `work/parallel-gs/build-gcc.log`.
- All wall times are upper bounds: the S1, S2 and S7 sessions ran a PS2Recomp compile, Ghidra and PCSX2 on
  the same 12-thread machine at the same time (their notes report load averages of 20 to 40).
- Granite's CMake is otherwise quiet with CMake 4.4.3 apart from two `cmake_minimum_required` deprecation
  warnings (Granite and muFFT). — evidence: `work/parallel-gs/configure-gcc.log`.

**GPU check on the RTX 4060**

The README lists eight requirements; the code checks more (`gs/gs_renderer.cpp` lines 798-830). Every one holds
on this GPU. — evidence: `work/parallel-gs/vulkaninfo-full.txt` (line numbers below) and the sandbox runs.

| Requirement | Source | RTX 4060 |
|---|---|---|
| `descriptorIndexing` | README and code | true (line 2752) |
| `timelineSemaphore` | README and code | true (2780) |
| `bufferDeviceAddress` | **code only** | true (2781) |
| `storageBuffer8BitAccess` | README and code | true (2745) |
| `storageBuffer16BitAccess` | README and code | true (2728) |
| `shaderInt16` | README and code | true (2174) |
| `scalarBlockLayout` | README and code | true (2774) |
| subgroup ops basic, vote, arithmetic, ballot, shuffle (README says "full minus clustered") | code | all 11 ops reported, clustered included (1338-1349) |
| subgroup size control with full subgroups, sizes 4 to 64 (README says 16 to 64) | code: `supports_subgroup_size_log2(true, 2, 6)` | `subgroupSizeControl` and `computeFullSubgroups` true (2800-2801); min = max = 32 (1432-1433); `requiredSubgroupSizeStages` covers compute |
| `maxComputeSharedMemorySize` >= 32 KiB | **code only** | 49,152 (779) |
| 16x super-sampling needs sizes 16 to 64 (`get_max_supported_super_sampling`) | code | satisfied: `SuperSampling::X16` available |

- Granite creates its own `VkInstance` at the highest version the loader offers (1.1 to 1.4; here 1.4.351) and
  opts into 22 optional device extensions on this driver, among them external memory and semaphore fd, dma-buf
  and `VK_EXT_external_memory_host`; useful if the scanout image is ever shared with a presentation device.
  The required features are Vulkan 1.2 and 1.3 core features, so "Vulkan 1.3 capable" in tech-stack.md §4.2 is
  the right public statement; whether Granite also accepts a 1.1 or 1.2 driver with the equivalent extensions
  is not tested here (inferred from `Granite/vulkan/context.cpp`). — evidence: `work/parallel-gs/sandbox-gcc.log`.
- Proof on the GPU: `parallel-gs-sandbox` from both compilers created the device, passed `GSInterface::init`,
  compiled all pipelines, wrote a synthetic dump and replayed it (exit 0; 3.8 s wall with GCC, 5.1 s with
  Clang). The first run spent about 1.4 s in seven synchronous pipeline compiles ("Stalled compile" warnings),
  the second run none, so the driver's shader cache absorbed them. The wrapper should persist Granite's pipeline
  cache (`Device::init_pipeline_cache` / `get_pipeline_cache_data`) so first launch pays this once (inferred).
  — evidence: `work/parallel-gs/sandbox-gcc.log`, `sandbox-clang.log`.

**Embedding surface** (from `gs/gs_interface.hpp`, `gs/gs_renderer.hpp`, `dump/gs_dump_parser.cpp` and the
PCSX2 integration patch in `misc/arch-pkgbuild-pcsx2/`, described in my own words)

There is an embedding API after all: the `ParallelGS::GSInterface` class, about 25 public methods, plus a handful
of plain structs. The PCSX2 integration and all three tools use nothing else.

1. **Device.** Granite's `Vulkan::Context` and `Vulkan::Device`. `Context::init_loader(vkGetInstanceProcAddr or
   null)` loads volk; `init_instance_and_device(no extensions, flags)` creates instance and device headless (the
   tools pass push-descriptor, descriptor-heap and descriptor-buffer flags); `set_num_thread_indices(n)` sizes
   per-thread command pools. Then `Device::set_context`, `Device::init_frame_contexts(4 in the tools, 12 in
   PCSX2)`. For interop, `Context::set_instance_factory` / `set_device_factory` let the host create the
   `VkInstance` and `VkDevice` itself from Granite's requested create-info (the first `pNext` link must be a
   `VkPhysicalDeviceFeatures2`), and `Device::set_queue_lock` serialises a shared queue. So our presentation
   pipeline (architecture.md §6.3) can share one `VkDevice` with the backend instead of adopting Granite's WSI;
   PCSX2 took the other route and implements Granite's `WSIPlatform` (seven virtuals).
2. **Init and settings.** `GSInterface::init(&device, GSOptions)` runs the requirement check above and allocates
   VRAM (`vram_size`, 4 MiB), the CLUT cache and the primitive buffers (64k primitives per flush).
   `GSOptions` also carries the super-sampling rate (X1 to X16, ordered grid or not, super-sampled textures,
   `dynamic_super_sampling` to allow `set_super_sampling_rate` at run time). `Hacks` is four opt-in
   inaccuracies (disable mipmaps, unsynced readbacks, backbuffer promotion, blend demotion); `DebugMode`
   drives RenderDoc-style capture and timestamps. `reset_context_state()` is the GS reset.
3. **Register writes.** `write_register(RegisterAddr, uint64)` for the general (A+D) registers; it is the same
   entry point the GIF packed and reglist decoders use, so a direct register write and a GIF packet are
   indistinguishable to the backend. The **privileged register page** (the `0x12000000` block) has no write
   method: `get_priv_register_state()` returns a struct laid out exactly like the 8 KiB page (PMODE, SMODE1/2,
   SRFSH, SYNCH1/2, SYNCV, DISPFB1/2, DISPLAY1/2, EXTBUF, EXTDATA, EXTWRITE, BGCOLOR at 16-byte stride in the
   first 4 KiB; CSR, IMR, BUSDIR, SIGLBLID in the second). PCSX2 aliases its GS register memory onto that
   struct and copies it in once per vsync. Our runtime can back the guest's privileged page with the struct, or
   copy at vsync. parallel-gs never touches CSR or IMR: FINISH, SIGNAL, LABEL and vsync interrupt bits are the
   host's job, with `SignalInterface::on_signal / on_finish / on_label` as the only callbacks (returning true
   forces a flush).
4. **GIF transfers.** `gif_transfer(path, data, size)` consumes GIFtag-prefixed quadwords (size a multiple of 16)
   with per-path tag state (`GIFPath` tag, reg and loop counters, readable through `get_gif_path` for save
   states); PACKED, REGLIST and IMAGE modes are decoded, with fast paths for the common vertex formats. There
   are four path slots; PCSX2 feeds its already-merged GS thread stream through slot 3, so we may keep PATH1,
   PATH2 and PATH3 separate (slots 0 to 2) if our DMA model arbitrates them, or feed one merged stream.
5. **Host to local and local to host.** Image uploads arrive through IMAGE packets inside `gif_transfer`.
   Direct VRAM access: `map_vram_write(offset, size)` plus `end_vram_write` (host writes into the shadow
   VRAM; used by save-state restore and by the dump reader), `map_vram_read(offset, size)` (blocks until the
   GPU timeline covers those pages, then returns the host copy), and `read_transfer_fifo(data, n)` for
   TRXDIR local-to-host reads (served from a readback buffer filled at flush; zeros past the end).
   `hacks.unsynced_readbacks` replaces the wait with zeros.
6. **Flush and scanout.** `flush()` closes the batch and submits. `vsync(VSyncInfo)` scans the CRTC circuits
   out into a fresh `R8G8B8A8_UNORM` image and returns a `ScanoutResult`: the Granite `ImageHandle` (left in
   the layout, stage and access the caller asked for), image size, internal size, mode size, interlaced flag,
   field phase, double-strike flag and pixel clock. `VSyncInfo` chooses force-progressive, overscan,
   anti-blur, CRTC offsets, high-resolution scanout and `skip_deinterlace` (raw fields, deinterlace yourself).
   `vsync_can_skip` tells the host when nothing changed, so a duplicated frame need not be scanned out again.
   `consume_flush_stats()` and `get_accumulated_timestamps()` feed an overlay. The dump reader is the smallest
   complete driver of this loop: `dump/gs_dump_parser.cpp` is 263 lines.
7. **Threading.** `GSInterface` has no internal locking: every call above must come from one thread (PCSX2
   calls it from its GS thread). The renderer owns one helper thread that waits on the timeline semaphore and
   publishes progress, and a few `std::async` futures for pipeline compilation at init. Granite's `Device`
   submission is thread-safe, but `set_context` and `init_frame_contexts` are setup-thread only, and the
   renderer calls `device->next_frame_context()` itself inside its submit path, so the host must not pace frame
   contexts on that device. Host readbacks (`map_vram_read`) block the calling thread. This fits
   architecture.md §12 if the machine thread hands GS work to a GS thread through a queue; parallel-gs does
   not provide that queue (PCSX2's MTGS does).
8. **Save states.** `get_register_state()`, `get_gif_path(i)`, `get_priv_register_state()`, `map_vram_read`
   and `map_vram_write`, then `clobber_register_state()` after a restore: exactly what the dump reader's
   `restart()` does. PCSX2's freeze and defrost are about 170 lines.

**Wrapper estimate.** PCSX2's integration is a 723-line `GSRendererPGS.cpp` plus a 71-line header and about
300 lines of dispatch, covering device bring-up through Granite WSI, presentation with FSR 1 and RCAS, save
states, settings and dump capture. Our `IGsBackend` implementation plus the C ABI shim: **1,000 to 1,500 lines
of C++** (device bring-up with a `DeviceFactory` for a shared `VkDevice` 150 to 250; register, GIF, transfer and
privileged-page plumbing about 150; vsync, image hand-off to the presentation pass and readback about 300;
settings, capabilities and diagnostics about 150; save state about 150; C ABI about 200). Inferred from the
PCSX2 integration's size; the trace backend and tests are extra.

**Replayer and the local patch**

- Upstream `parallel-gs-replayer` writes nothing to disk: it prints "Total time per VBlank" and triggers a
  RenderDoc capture if RenderDoc launched it. It also opens `argv[1]` instead of the parsed path, so any option
  placed before the dump path breaks it. — evidence: `tools/gs_dump_replayer.cpp` line 103 upstream.
- Local patch `work/parallel-gs/patches/0001-replayer-dump-png.patch` (225-line diff; candidate for
  `patches/parallel-gs/` in the repo, it modifies an LGPL file so it stays LGPL-3.0+ and carries no game data):
  `--dump-png <dir>` writes `frame_NNNNN.png` for every vsync of the first iteration plus `frames.csv` (image
  size, internal and mode size, interlace flag and phase, pixel clock, CPU and total ms, primitive, render-pass,
  copy and palette counts), `--max-vsyncs <n>`, `--raw` for the headerless stream format, and the path fix;
  links `granite-stb`. The readback is the same barrier, copy-to-buffer, wait-idle, map sequence as the upstream
  repro tool.
- Verified on the synthetic dump (`work/parallel-gs/sandbox-test.gs`, 4.2 MB, written by the sandbox tool):
  one 640x448 progressive frame at 1x, 2x and 4x super-sampling (`frames-sandbox/ssaa*/`). First-frame replay
  took 0.76 s, 1.16 s and 1.42 s, almost all pipeline compilation. Steady state with `--iterations 5`: 1.19 ms
  per vblank at 1x, 1.96 ms at 4x, 2.07 ms at 16x, with 576 to 720 MiB of device memory tracked. The scene has
  two primitives, so these numbers prove the pipeline, not performance. — evidence:
  `work/parallel-gs/replayer-sandbox-*.log`, `frames-sandbox/*/frames.csv`.

**Dump reader facts that correct the brief**

- Accepted state versions are **8 to 9** (`STATE_VERSION_MIN = 8`, `STATE_VERSION = 9`), not "9 or newer":
  anything else fails `open()`. The file must start with the FakeCRC marker (`0xFFFFFFFF`) and the header; there
  is no decompression, so S7 must capture with `GSDumpCompression` set to uncompressed. ReadFIFO packets are
  logged as unimplemented and end the current iteration. `open_raw()` reads the headerless stream that the
  `misc/` PCSX2 patch pipes through a FIFO. — evidence: `dump/gs_dump_parser.hpp` lines 14-16,
  `dump/gs_dump_parser.cpp` lines 14-25 and 197-203.
- The README's "version 8" and the brief's "≥ 9" were both half right; PCSX2's current dumps are version 9 and
  parallel-gs skips the 61 extra bytes they carry.

**LGPL shared-library arrangement**

- `libparallel-gs.a` only references the `Vulkan::` (82 symbols) and `Util::` (7) namespaces, so the library
  proper needs `granite-vulkan`, `granite-util`, `granite-math` and `granite-volk`; the extra Granite
  application, event, filesystem, threading, path and input archives on the tools' link lines come from
  `granite-base`, which only the tools link. — evidence: `nm -C --undefined-only
  work/parallel-gs/build-gcc-standalone/gs/libparallel-gs.a`.
- Plan that satisfies LGPL-3.0 §4: one shared library (`dq8-gs-parallel`) containing our wrapper (GPL-3.0-or-later,
  the only exported symbols are the C ABI), parallel-gs and the four Granite archives, all compiled with hidden
  visibility (`GRANITE_HIDDEN` exists for exactly this: "build Granite as a static library and link it in your
  shared library"). We ship the LGPL text and notice, and the complete source and build scripts of that library
  so a user can rebuild and relink it with a modified parallel-gs; because our wrapper is GPL its source ships
  anyway. The rest of the host links only the C ABI, never Granite or parallel-gs symbols. No user-side build is
  involved: the library is part of the redistributable host. The tools (replayer, stream, repro) and their
  shaderc toolchain stay contributor-only.
- The dependency set that reaches the shared library is MIT and Apache-2.0 or MIT, all GPL-3.0 compatible;
  the tools additionally pull shaderc (BSD-3-Clause), glslang (BSD-2-Clause and others), SPIRV-Tools and
  SPIRV-Cross (Apache-2.0), stb (MIT or public domain), rapidjson (MIT, with a BSD-licensed `msinttypes`
  directory), meshoptimizer (MIT), muFFT (MIT) and FSR 2 (MIT). — evidence: the LICENSE files under
  `work/parallel-gs/Granite/third_party/`.

### What changes in the docs

- tech-stack.md §4.2: the parallel-gs feature row gains `bufferDeviceAddress`, at least 32 KiB of compute
  shared memory, the five subgroup operations (basic, vote, arithmetic, ballot, shuffle; clustered is not
  needed), and subgroup size control with full subgroups for sizes 4 to 64 (16 to 64 only for 16x
  super-sampling). Add "verified on the RTX 4060 on 2026-10-19 (S5a)". Keep "Vulkan 1.3 capable".
- tech-stack.md §5, paraLLEl-GS row: pin `cc6184a` (2026-10-06) and Granite `16e7395`; replace "No documented
  embedding API: expect to write a wrapper" with "Embedding API is the `GSInterface` class; wrapper estimated at
  1,000 to 1,500 lines (S5a)"; note C++14, the precompiled SPIR-V header, that the library build needs three
  submodules and no shader toolchain, and that the tools (contributor-only) need ten more plus SDL3 and build
  the SPIR-V toolchain (about 7.5 minutes).
- tech-stack.md §12: add paraLLEl-GS tools built under `work/parallel-gs/` to the "Found" list, and note that
  RenderDoc is optional for the replayer.
- architecture.md §6.1: name the privileged register page explicitly in "The backend is told", and map "image
  data uploads to and downloads" to `map_vram_write`, `map_vram_read` and `read_transfer_fifo`.
- architecture.md §6.2, parallel-gs row: "No documented embedding API: we write a wrapper" becomes "Small
  embedding API (`GSInterface`), proven headless in S5a; wrapper 1 to 1.5k lines"; add "needs
  `bufferDeviceAddress` and 32 KiB shared memory" to the limits; add "all calls on one thread; the renderer owns
  one helper thread".
- architecture.md §12: state that the GS backend is driven from exactly one thread and that host readbacks block
  that thread, which the machine thread design must allow for.
- project-brief.md §6 (facts observed) and §14 D5: add the S5a evidence row.
- Docs/spikes/S5 brief "Why": dump state version 8 to 9, uncompressed.

### Decision impact

- D5 / ADR-0004: S5a removes the build, GPU-feature and "no API" risks: both compilers build it without a patch,
  the reference GPU meets every requirement including the undocumented ones, and the embedding surface is
  small and already exercised by PCSX2. Recommendation: keep paraLLEl-GS as the first backend and write the
  ADR after S5b shows parity on real frames; parity and real-frame timing are the remaining risk, and the LGPL
  shared-library arrangement is a constraint, not a blocker.
- R3 (GS speed and accuracy): unchanged until S5b; the synthetic timings only prove that the pipeline runs.
- No change to D3, D4, D6, D7 or D8.

### Local artifacts (git-ignored)

- `work/parallel-gs/`: the clone with submodules (759 MB including builds).
- `work/parallel-gs/build-{gcc,clang}-standalone/`, `build-{gcc,clang}/`: the four build trees.
- `work/parallel-gs/configure-*.log`, `build-*.log`, `build-gcc-resume.log`: configure and build logs.
- `work/parallel-gs/vulkaninfo.txt`, `vulkaninfo-full.txt`: `vulkaninfo --summary` and the full report.
- `work/parallel-gs/sandbox-{gcc,clang}.log`: the GPU proof runs.
- `work/parallel-gs/sandbox-test.gs`: synthetic dump written by `parallel-gs-sandbox` (no game data).
- `work/parallel-gs/frames-sandbox/ssaa{1,2,4}/frame_00000.png` and `frames.csv`: replayer output.
- `work/parallel-gs/replayer-sandbox-*.log`: replayer runs.
- `work/parallel-gs/patches/0001-replayer-dump-png.patch`: the local replayer patch (applied in the tree).

### Open questions

- Does Granite accept a Vulkan 1.1 or 1.2 driver that exposes the needed features through extensions, or is
  1.3 required in practice? → check with lavapipe in CI (M1) or on a Steam Deck class device.
- `std::is_pod` in `gs_interface.hpp` is deprecated in C++20; expect a warning, not an error, in our C++20
  wrapper translation unit → confirm when the wrapper is written (M3).
- Shared `VkDevice` through `DeviceFactory` versus adopting Granite's WSI for the window → wrapper design (M3).
- Interrupt delivery: parallel-gs never raises CSR bits, so FINISH, SIGNAL and vsync interrupts come from our
  GS model at safe points (architecture.md §12) → runtime design (M2, M3).
- Parity and real timing on this game's frames → S5b.

### S5b steps, ready for when S7's dumps exist

1. Capture with `GSDumpCompression` uncompressed (the reader has no decompressor) and check the header version
   is 8 or 9; the patched replayer prints a clear error otherwise.
2. Per dump and rate: `parallel-gs-replayer <dump> --ssaa <1|2> --dump-png work/parallel-gs/frames/<scene>/ssaa<n>`
   (binary in `work/parallel-gs/build-gcc/tools/`); add `--conservative-crtc` for raw fields with overscan and
   CRTC offsets when matching PCSX2's software-renderer geometry, and `--high-res-scanout` for the enhanced look.
3. `--iterations 5` without `--dump-png` for the steady-state ms per vblank; the "Memory usage" lines give the
   GPU memory per scene.
4. Compare against PCSX2's software-renderer screenshot of the same vsync with
   `ffmpeg -i ours.png -i ref.png -filter_complex ssim -f null -`; explain systematic differences by
   deinterlacing (weave versus PCSX2's mode; use `--conservative-crtc` to compare raw fields), field blending and
   anti-blur, and AA1.
5. Record per scene (title, field, menu, battle): resolution, interlaced or not, ms per vblank at 1x, 2x and 4x,
   SSIM, GPU memory; then write ADR-0004.
6. S7's progress note already shows what to expect: PCSX2 writes state version 9 uncompressed dumps (in range),
   and the game runs at 512x448 NTSC interlaced in FIELD mode with a 30 Hz update, so every S5b replay goes
   through the deinterlacer and the field-aware super-sampling path; compare raw fields (`--conservative-crtc`)
   before judging the weave output, and note that `adapt_to_internal_horizontal_resolution` is the switch that
   avoids the extra horizontal blur on 512-wide frames.
