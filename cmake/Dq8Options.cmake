# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# Build options. Keep this the single place where options are declared.
option(DQ8_BUILD_TESTS "Build the unit tests (Catch2)" ON)
option(DQ8_WERROR "Treat compiler warnings as errors" OFF)
option(DQ8_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(DQ8_USE_SYSTEM_TOML11 "Use an installed toml11 instead of the submodule" OFF)
option(DQ8_USE_SYSTEM_CATCH2 "Use an installed Catch2 instead of the submodule" OFF)
option(DQ8_BUILD_RECOMP "Build ps2_recomp from the pinned third_party/PS2Recomp (cmake/Ps2Recomp.cmake)" OFF)
option(DQ8_BUILD_E2E "Build the end-to-end recompile tests (label e2e; needs DQ8_BUILD_RECOMP)" OFF)
# Builds tests/abi/sample_unit through cmake/module and loads it. Off on Windows until the loader test and the
# template's MSVC path are verified there.
if(WIN32)
    set(_dq8_abi_module_test_default OFF)
else()
    set(_dq8_abi_module_test_default ON)
endif()
option(DQ8_BUILD_ABI_MODULE_TEST "Build the ABI sample module through cmake/module and test loading it"
       ${_dq8_abi_module_test_default})
set(DQ8_SANITIZERS "" CACHE STRING "Comma-separated -fsanitize= list, e.g. address,undefined (GCC/Clang only)")

get_property(_dq8_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _dq8_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(DQ8_BUILD_E2E AND NOT (DQ8_BUILD_RECOMP AND DQ8_BUILD_TESTS))
    message(FATAL_ERROR "DQ8_BUILD_E2E needs DQ8_BUILD_RECOMP=ON and DQ8_BUILD_TESTS=ON")
endif()

# M3 (Docs/milestones/M3-boots.md). Fibers: the backend behind rt::Fiber (ADR-0007).
set(DQ8_FIBER_BACKEND "minicoro" CACHE STRING "Fiber backend for guest threads: minicoro, ucontext or win32")
set_property(CACHE DQ8_FIBER_BACKEND PROPERTY STRINGS minicoro ucontext win32)
# The SDL3 window and input of dq8-game (ADR-0005). The submodule is the default; an installed SDL3 builds faster.
option(DQ8_BUILD_WINDOW "Build dq8-game's SDL3 window, input and presentation" ON)
option(DQ8_USE_SYSTEM_SDL3 "Use an installed SDL3 instead of the third_party/SDL submodule" OFF)
# The paraLLEl-GS backend as the shared library dq8-gs-parallel (ADR-0004). Its MSVC build is unverified in M3.
if(MSVC)
    set(_dq8_gs_parallel_default OFF)
else()
    set(_dq8_gs_parallel_default ON)
endif()
option(DQ8_BUILD_GS_PARALLEL "Build the paraLLEl-GS backend library dq8-gs-parallel (needs Vulkan at run time)"
       ${_dq8_gs_parallel_default})
# The GUI launcher dq8-launcher (M5, ADR-0010): Dear ImGui over SDL3, so it needs the SDL3 build of the window.
option(DQ8_BUILD_LAUNCHER "Build the dq8-launcher GUI (Dear ImGui over SDL3; needs DQ8_BUILD_WINDOW)" ON)
if(DQ8_BUILD_LAUNCHER AND NOT DQ8_BUILD_WINDOW)
    message(STATUS "DQ8_BUILD_LAUNCHER is ignored: it needs DQ8_BUILD_WINDOW=ON")
    set(DQ8_BUILD_LAUNCHER OFF CACHE BOOL "" FORCE)
endif()
