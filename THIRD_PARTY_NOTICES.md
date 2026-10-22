# Third-party notices

Every dependency this project bundles or links, with its license. Update this file in the same commit that adds
or bumps a dependency (tech-stack.md §10). Licenses of bundled code are reproduced in each submodule's own tree.
Only the nested submodules `scripts/init_submodules.sh` initialises are bundled; paraLLEl-GS's other nested
repositories are never fetched.

## Bundled (git submodules under `third_party/`)

| Component | Version | Commit | License | Used for |
|---|---|---|---|---|
| [toml11](https://github.com/ToruNiina/toml11) | v4.4.0 | `be08ba2be2a964edcdb3d3e3ea8d100abc26f286` | BSL-1.0 | Configuration, region profiles, settings |
| [Catch2](https://github.com/catchorg/Catch2) | v3.16.1 | `08092139210881f01a34e60c5f2fd6afa0ea4024` | BSL-1.0 | Unit tests (not shipped in releases) |
| [PS2Recomp](https://github.com/ran-j/PS2Recomp) | eb8688b | `eb8688b2fd191129f103a39543002c005b33409c` | GPL-3.0 | The recompiler (`ps2_recomp`), built from source and shipped with the host (ADR-0003); `include/dq8/abi/ps2recomp/` adapts its context and macro headers (GPL-3.0-only, attributed) |
| [ELFIO](https://github.com/serge1/ELFIO) | Release_3.12 | `8ae6cec5d60495822ecd57d736f66149da9b1830` | MIT | ELF reader inside `ps2_recomp` |
| [fmt](https://github.com/fmtlib/fmt) | 12.1.0 | `407c905e45ad75fc29bf0f9bb7c5c2fd3475976f` | MIT | Formatting inside `ps2_recomp` |
| [libdwarf](https://github.com/davea42/libdwarf-code) | v2.2.0 | `2e088b983562860f497b4fd789a3e61524eb857c` | LGPL-2.1 (library) | DWARF reader inside `ps2_recomp` |
| [rabbitizer](https://github.com/Decompollaborate/rabbitizer) | 1.14.3 | `d5804fa488474830a47fbf9f3693048ec1e45589` | MIT | MIPS disassembler inside `ps2_recomp` |
| [minicoro](https://github.com/edubart/minicoro) | v0.1.3+3 | `02dad0f8b7cbb12fe6e216ae7a76db15ca55cd7b` | MIT-0 or Unlicense | Fibers for guest threads (ADR-0007), header-only |
| [SDL](https://github.com/libsdl-org/SDL) | release-3.4.18 | `829a65d769d935c4852f8159e964312c0957260a` | Zlib | Window, input and gamepads in `dq8-game` (ADR-0005); `DQ8_USE_SYSTEM_SDL3` uses an installed SDL3 instead |
| [stb](https://github.com/nothings/stb) | master | `a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d` | MIT or Unlicense | `stb_image_write.h` for frame dumps, header-only |
| [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) | cc6184a | `cc6184af7e0c03da603045ca371ffa5dae9b0655` | LGPL-3.0-or-later | First GS backend (ADR-0004), built into the separate shared library `dq8-gs-parallel` so the LGPL relinking terms hold |
| [Granite](https://github.com/Themaister/Granite) (nested in paraLLEl-GS) | 16e7395 | `16e7395f6a4858c1783dbf6f521f90b9d5f82ac5` | MIT | Vulkan device layer of paraLLEl-GS; only the vulkan, util and math parts are built |
| [volk](https://github.com/zeux/volk) (nested in Granite) | 1.4.350-4 | `47cddf7ed97b94118a08aacb548a411188e016cc` | MIT | Vulkan loader for `dq8-gs-parallel` |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) (nested in Granite) | v1.4.358 | `11d6898377797e07dbd543aaaa367e4465074597` | Apache-2.0 or MIT | Vulkan API headers for `dq8-gs-parallel` |

## Adapted into first-party source files (not submodules)

| Component | Version | Commit | License | Used for |
|---|---|---|---|---|
| [PCSX2](https://github.com/PCSX2/pcsx2) `pcsx2/SPU2/` | v2.7.524 | `3e29183a37e74cbc8c17bda8afb63c2d9bc6fd14` | GPL-3.0-or-later | The SPU2 model in `src/runtime/spu2/` (ADR-0014): registers, ADPCM, the decode FIFO and Gaussian table, envelopes, volume sweeps, noise, the mix, reverb and its resampling filters, DMA and AutoDMA; each adapted file keeps GPL-3.0-or-later and names the PCSX2 Dev Team |

`src/runtime/movie/ipu_csc.cpp` adapts PCSX2's IPU colour conversion (PCSX2 Dev Team, GPL-3.0-or-later, attributed
in the file); the movie HLE's elementary-stream model, macroblock layout and `dq8-movie` decoder set-up adapt
PS2Recomp's `MPEG.cpp` (GPL-3.0-only, attributed in each file).

## System libraries linked at build time, not bundled

| Component | Version | License | Used for |
|---|---|---|---|
| [FFmpeg](https://ffmpeg.org) libavcodec and libavutil | the system's (Ubuntu 24.04: 6.1, libavcodec 60; also built with 8.0, libavcodec 63) | LGPL-2.1-or-later (GPL-2.0-or-later when the system's build enables GPL parts) | The `mpeg2video` decoder inside the separate shared library `dq8-movie` (ADR-0015), which `dq8-game` loads at run time and a user can replace; `DQ8_FFMPEG=auto\|system\|off` (`cmake/Movie.cmake`). Dynamically linked, never copied into this repository; a bundled LGPL build comes with packaging (M5) |

## Tools used during development, not bundled or redistributed

| Tool | License | Role |
|---|---|---|
| [Ghidra](https://github.com/NationalSecurityAgency/ghidra) 12.1.4 | Apache-2.0 | Static analysis of the user's boot ELF (contributors only) |
| [ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded) 2.1.38 | Apache-2.0 | Emotion Engine (R5900) support for Ghidra |
| [REA](https://github.com/morluto/rea) 6.7.0 | MIT | Agent-facing MCP server and CLI over Ghidra |
| [PCSX2](https://github.com/PCSX2/pcsx2) 2.7.524 | GPL-3.0 | Reference emulator: behaviour ground truth, GS dumps, save states |

The game itself, its executable, data, movies, and the PS2 BIOS are the property of their rights holders and are
never part of this project. See project-brief.md §9.
