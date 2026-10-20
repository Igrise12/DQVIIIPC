# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# Runs a program and checks both its exit status and its output (CTest's PASS_REGULAR_EXPRESSION would ignore
# the exit status). Usage:
#   cmake -DDQ8_GAME=<exe> "-DARGS=a@@b@@c" -DEXPECT_EXIT=<n> "-DEXPECT=<regex>@@<regex>" -P run_and_match.cmake
# Lists are separated by "@@" because add_test would split a ";" list into separate arguments. Every regex
# must match stdout plus stderr.
string(REPLACE "@@" ";" ARGS "${ARGS}")
string(REPLACE "@@" ";" EXPECT "${EXPECT}")
execute_process(COMMAND "${DQ8_GAME}" ${ARGS}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(all "${out}${err}")
if(NOT "${rc}" STREQUAL "${EXPECT_EXIT}")
    message(FATAL_ERROR "exit status ${rc}, expected ${EXPECT_EXIT}\n--- output ---\n${all}")
endif()
foreach(pattern IN LISTS EXPECT)
    if(NOT all MATCHES "${pattern}")
        message(FATAL_ERROR "output does not match '${pattern}'\n--- output ---\n${all}")
    endif()
endforeach()
message(STATUS "exit ${rc}, output matched:\n${all}")
