# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M3a acceptance 1 (Docs/milestones/M3-boots.md): boot the user's disc to the title screen, headless, with
# the trace backend and our scripted input. The run must exit 0 with --until met, and its events must show
# TITLE resident through the hook, mode 4, and no guest stop. Skips (prints "SKIPPED:") when DQ8_TEST_ISO is
# unset or the game modules are missing.
#   cmake -DDQ8_GAME=<exe> -DSCRIPT=<.dq8in> -DMODULE_DIR=<default> -DWORK_DIR=<dir> -P run_boot_to_title.cmake
cmake_minimum_required(VERSION 3.25)

set(iso "$ENV{DQ8_TEST_ISO}")
if(iso STREQUAL "")
    message("SKIPPED: DQ8_TEST_ISO is not set")
    return()
endif()
set(modules "$ENV{DQ8_TEST_MODULE_DIR}")
if(modules STREQUAL "")
    set(modules "${MODULE_DIR}")
endif()
if(NOT IS_DIRECTORY "${modules}")
    message("SKIPPED: no game modules in ${modules} (dq8ctl recomp all; or set DQ8_TEST_MODULE_DIR)")
    return()
endif()

file(MAKE_DIRECTORY "${WORK_DIR}")
set(events "${WORK_DIR}/boot-to-title.events.jsonl")
execute_process(
    COMMAND "${DQ8_GAME}" --iso "${iso}" --module-dir "${modules}" --headless --gs trace
            --input-script "${SCRIPT}" --until title --vblanks 6000 --events "${events}" --json
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
string(REGEX MATCH "[^\n]*$" last_line "${out}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "dq8-game exited ${rc}, expected 0\n--- report\n${out}\n--- stderr (tail)\n${err}")
endif()
file(READ "${events}" log)
foreach(pattern IN ITEMS "\"ev\":\"overlay_resident\"[^\n]*\"name\":\"TITLE\"" "\"ev\":\"mode\"[^\n]*\"mode\":4[,}]"
                         "\"ev\":\"end\"[^\n]*\"ended\":\"until\"")
    if(NOT log MATCHES "${pattern}")
        message(FATAL_ERROR "the events in ${events} do not match '${pattern}'")
    endif()
endforeach()
if(out MATCHES "\"stop\":")
    message(FATAL_ERROR "the run reports a guest stop:\n${out}")
endif()
message(STATUS "boot to title: ok (events in ${events})")
