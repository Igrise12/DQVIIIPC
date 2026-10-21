# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# Pinned third-party dependencies (git submodules). Each one is listed in THIRD_PARTY_NOTICES.md.
if(DQ8_USE_SYSTEM_TOML11)
    find_package(toml11 CONFIG REQUIRED)
else()
    add_subdirectory(toml11 EXCLUDE_FROM_ALL)
    # Our warning set is stricter than toml11's; treat its headers as system headers so they never trip
    # DQ8_WERROR (clang 20+ warns about its literal operators, for instance).
    get_target_property(_dq8_toml11_includes toml11 INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(toml11 PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_dq8_toml11_includes}")
endif()

if(DQ8_BUILD_TESTS)
    if(DQ8_USE_SYSTEM_CATCH2)
        find_package(Catch2 3 CONFIG REQUIRED)
    else()
        add_subdirectory(Catch2 EXCLUDE_FROM_ALL)
        list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/Catch2/extras")
        set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" PARENT_SCOPE)
    endif()
endif()

# Header-only dependencies as INTERFACE targets with system include directories (no warnings from their code).
# minicoro's implementation is compiled in exactly one translation unit of ours (MINICORO_IMPL, ADR-0007).
add_library(dq8_minicoro INTERFACE)
add_library(dq8::minicoro ALIAS dq8_minicoro)
target_include_directories(dq8_minicoro SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/minicoro")
# stb: only stb_image_write.h is used, compiled in one translation unit (STB_IMAGE_WRITE_IMPLEMENTATION).
add_library(dq8_stb INTERFACE)
add_library(dq8::stb ALIAS dq8_stb)
target_include_directories(dq8_stb SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/stb")
# SDL3 (DQ8_BUILD_WINDOW) and paraLLEl-GS (DQ8_BUILD_GS_PARALLEL) are wired up in cmake/Sdl3.cmake and
# cmake/ParallelGs.cmake (M3 session D).
if(DQ8_BUILD_WINDOW)
    include(Sdl3)
endif()
if(DQ8_BUILD_GS_PARALLEL)
    include(ParallelGs)
endif()
