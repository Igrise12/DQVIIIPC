# dq8-gs-parallel: notice

`dq8-gs-parallel` (`dq8-gs-parallel.so` on Linux, `dq8-gs-parallel.dll` on Windows) is the paraLLEl-GS backend of
DQ8-Native. It is one shared library made of:

| Part | License | Source |
|---|---|---|
| Our wrapper, `src/runtime/gs/parallel/gs_parallel.cpp` | GPL-3.0-or-later | this repository |
| [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs), commit `cc6184a` | LGPL-3.0-or-later | `third_party/parallel-gs` |
| [Granite](https://github.com/Themaister/Granite) (vulkan, util, math), commit `16e7395` | MIT | `third_party/parallel-gs/Granite` |
| [volk](https://github.com/zeux/volk) | MIT | `third_party/parallel-gs/Granite/third_party/volk` |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) (headers only) | Apache-2.0 or MIT | `third_party/parallel-gs/Granite/third_party/khronos/vulkan-headers` |

The full text of the GNU Lesser General Public License version 3 ships next to the library as
`dq8-gs-parallel.COPYING.LGPLv3` (and in an install under `share/doc/dq8-native/dq8-gs-parallel/`). The licenses of
Granite, volk and Vulkan-Headers are in their source trees; `THIRD_PARTY_NOTICES.md` lists them all.

## Using a modified paraLLEl-GS

The rest of DQ8-Native never links paraLLEl-GS or Granite: `dq8-game` loads this library at run time and calls only
the nine `dq8_gsp_*` functions of `include/dq8/gs/gs_parallel_abi.h` (C ABI version 1). To run DQ8-Native with your
own version of paraLLEl-GS:

1. Get the DQ8-Native source of your release and run `scripts/init_submodules.sh`.
2. Change `third_party/parallel-gs` as you like (or point the submodule at your fork).
3. Build only this library: `cmake --preset release && cmake --build --preset release --target dq8-gs-parallel`.
4. Replace the installed `dq8-gs-parallel` file with `build/release/src/runtime/gs/parallel/dq8-gs-parallel.so`
   (`.dll` on Windows), or set `DQ8_GS_PARALLEL` to its path.

`dq8-game` checks the library's `dq8_gsp_abi_version()` and refuses a library of another ABI version.

## Diagnostics

| Variable | Effect |
|---|---|
| `DQ8_GSP_VERBOSE=1` | Prints Granite's and paraLLEl-GS's information messages (errors and warnings are always printed) |
| `DQ8_GSP_TIMESTAMPS=1` | Enables paraLLEl-GS's GPU timestamps; `dq8_gsp_frame_info::gpu_ns` then reports GPU time per vsync |
