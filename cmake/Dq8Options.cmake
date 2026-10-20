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
set(DQ8_SANITIZERS "" CACHE STRING "Comma-separated -fsanitize= list, e.g. address,undefined (GCC/Clang only)")

get_property(_dq8_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _dq8_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(DQ8_BUILD_E2E AND NOT (DQ8_BUILD_RECOMP AND DQ8_BUILD_TESTS))
    message(FATAL_ERROR "DQ8_BUILD_E2E needs DQ8_BUILD_RECOMP=ON and DQ8_BUILD_TESTS=ON")
endif()
