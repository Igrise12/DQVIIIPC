# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# abi.sample_module_exports: a game module exports dq8_game_module_describe and nothing else.
# Run as: cmake -DNM=<nm> -DMODULE=<library> -P check_module_exports.cmake
execute_process(COMMAND "${NM}" -D --defined-only "${MODULE}"
                OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${NM} failed on ${MODULE}: ${_err}")
endif()
string(REGEX MATCHALL "[^\n]+" _lines "${_out}")
set(_exports "")
foreach(_line IN LISTS _lines)
    # "<value> <type> <name>"; absolute symbols (type A) are version nodes, not exports
    if(_line MATCHES "^[0-9a-fA-F]* +([A-Za-z]) +([^ ]+)$" AND NOT CMAKE_MATCH_1 STREQUAL "A")
        list(APPEND _exports "${CMAKE_MATCH_2}")
    endif()
endforeach()
if(NOT _exports STREQUAL "dq8_game_module_describe")
    message(FATAL_ERROR "expected exactly one export, dq8_game_module_describe; ${MODULE} exports: ${_exports}")
endif()
message(STATUS "one export: ${_exports}")
