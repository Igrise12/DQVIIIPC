# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M5 session D: published function maps (test_published_maps*), the jump-table fill from a unit image, the
# checker's data/maps rules and the install layout.
target_sources(dq8_unit_tests PRIVATE test_published_maps.cpp)

# The checker's data/maps rules and release mode, and tools/ghidra/publish_maps.py (Python 3.11 for tomllib).
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME scripts.published_map_tools
        COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/test_published_map_tools.py")
    set_tests_properties(scripts.published_map_tools PROPERTIES LABELS unit)
else()
    message(STATUS "scripts.published_map_tools: off (no Python 3.11 or later)")
endif()
