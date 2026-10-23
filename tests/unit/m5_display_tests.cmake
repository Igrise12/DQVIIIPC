# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M5 session C: window modes, aspect and the 16:9 watcher, internal resolution, bindings, dead zones, port
# assignment and audio device and volume (test_display_*, test_bindings*, ...). Tests that link SDL3 go with
# dq8_platform_sdl_tests in CMakeLists.txt; the rest join dq8_unit_tests here.
target_sources(dq8_unit_tests PRIVATE test_display.cpp)

# dq8_platform_sdl_tests is defined further down CMakeLists.txt than this list is included, so its source is added
# once the directory has been read.
if(DQ8_BUILD_WINDOW)
    cmake_language(DEFER CALL target_sources dq8_platform_sdl_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test_display_sdl.cpp")
endif()
