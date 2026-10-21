# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# paraLLEl-GS (LGPL-3.0-or-later) and the parts of Granite it needs, built as static, position-independent archives
# with hidden visibility so that src/runtime/gs/parallel can link them into the shared library dq8-gs-parallel
# (ADR-0004, Docs/spikes/S5-parallel-gs-standalone.md). Only the library is configured
# (PARALLEL_GS_STANDALONE=ON: Granite platform "null", no SPIR-V toolchain, no renderer, no tools), which needs the
# three nested submodules scripts/init_submodules.sh initialises. The third-party targets get neither our warning
# set nor our sanitizers, and their include directories are SYSTEM for our code.
#
# Our changes to paraLLEl-GS, if any, live in patches/parallel-gs/ and are applied to the submodule by hand
# (patches/parallel-gs/README.md); none is needed for the library.
#
# Defines (when DQ8_BUILD_GS_PARALLEL is ON): the target `parallel-gs` and the granite-* archives it pulls in.

set(_dq8_parallel_gs_dir "${PROJECT_SOURCE_DIR}/third_party/parallel-gs")
if(NOT EXISTS "${_dq8_parallel_gs_dir}/Granite/third_party/volk/volk.h"
   OR NOT EXISTS "${_dq8_parallel_gs_dir}/Granite/third_party/khronos/vulkan-headers/include/vulkan/vulkan.h")
    message(FATAL_ERROR "paraLLEl-GS or its nested submodules are missing: run scripts/init_submodules.sh, "
                        "or configure with -DDQ8_BUILD_GS_PARALLEL=OFF")
endif()

# A function so that every variable below stays in this scope (and the subdirectories it adds).
function(_dq8_add_parallel_gs)
    # Granite asks for CMake 3.6, which leaves CMP0077 unset: make option() honour the normal variables below.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

    set(PARALLEL_GS_STANDALONE ON)
    set(GRANITE_HIDDEN ON)                # -fvisibility=hidden inside Granite
    set(GRANITE_POSITION_INDEPENDENT ON)  # the archives end up in a shared library
    set(GRANITE_INSTALL_TARGETS OFF)
    set(GRANITE_TOOLS OFF)
    set(GRANITE_RENDERDOC_CAPTURE OFF)    # nothing in paraLLEl-GS uses it; no librenderdoc probing at run time
    set(GRANITE_VULKAN_DXGI_INTEROP OFF)

    # paraLLEl-GS's own targets sit outside Granite's directory, so its flags do not reach them.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_C_VISIBILITY_PRESET hidden)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

    add_subdirectory("${_dq8_parallel_gs_dir}" "${PROJECT_BINARY_DIR}/third_party/parallel-gs" EXCLUDE_FROM_ALL SYSTEM)
endfunction()

_dq8_add_parallel_gs()

if(NOT TARGET parallel-gs)
    message(FATAL_ERROR "third_party/parallel-gs did not define the parallel-gs target")
endif()
