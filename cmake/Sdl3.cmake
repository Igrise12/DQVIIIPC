# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# SDL3 for dq8-game's window, input and presentation (ADR-0005). By default the pinned submodule third_party/SDL is
# built as a static library; DQ8_USE_SYSTEM_SDL3=ON uses an installed SDL3 (3.4 or newer) instead. Either way the
# result is the target SDL3::SDL3, whose include directories are SYSTEM for our code. SDL gets neither our warning
# set nor our sanitizers.

if(DQ8_USE_SYSTEM_SDL3)
    # GLOBAL: the imported targets must be visible to src/runtime/platform/sdl and the tests, not only here.
    find_package(SDL3 3.4 CONFIG REQUIRED COMPONENTS SDL3 GLOBAL)
    message(STATUS "SDL3: installed ${SDL3_VERSION}")
else()
    set(_dq8_sdl_dir "${PROJECT_SOURCE_DIR}/third_party/SDL")
    if(NOT EXISTS "${_dq8_sdl_dir}/CMakeLists.txt")
        message(FATAL_ERROR "third_party/SDL is missing: run scripts/init_submodules.sh, configure with "
                            "-DDQ8_USE_SYSTEM_SDL3=ON, or turn the window off with -DDQ8_BUILD_WINDOW=OFF")
    endif()

    function(_dq8_add_sdl3)
        set(SDL_SHARED OFF)
        set(SDL_STATIC ON)
        set(SDL_TEST_LIBRARY OFF)
        set(SDL_TESTS OFF)
        set(SDL_EXAMPLES OFF)
        set(SDL_INSTALL OFF)
        set(SDL_UNINSTALL OFF)
        add_subdirectory("${_dq8_sdl_dir}" "${PROJECT_BINARY_DIR}/third_party/SDL" EXCLUDE_FROM_ALL SYSTEM)
    endfunction()
    _dq8_add_sdl3()
    message(STATUS "SDL3: third_party/SDL (static)")
endif()

if(NOT TARGET SDL3::SDL3)
    message(FATAL_ERROR "SDL3 did not provide the SDL3::SDL3 target")
endif()
