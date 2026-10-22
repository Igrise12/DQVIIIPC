# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# host.dq8_game_card_wiring: dq8-game's memory-card options (M4 session D) on the synthetic machine program,
# which never calls the card server:
#   - in-memory cards by default headless: the end event carries card_sha1 (port 0) and no card2_sha1;
#   - --save-dir with two cards: both digests, the same for the same options, and no folder written while the
#     guest changed nothing;
#   - --memory-card none: no digest; an --rtc date that does not exist is a usage error (exit 2).
#   cmake -DDQ8_GAME=<exe> -DELF=<elf> -DMODULE_DIR=<dir> -DISO=<iso> -DWORK_DIR=<dir> -P run_card_wiring.cmake
cmake_minimum_required(VERSION 3.25)

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Runs dq8-game for 3 vblanks with extra options; sets <prefix>_rc and <prefix>_end (the end event's JSON).
function(run prefix)
    set(events "${WORK_DIR}/${prefix}.jsonl")
    execute_process(
        COMMAND "${DQ8_GAME}" --elf "${ELF}" --module-dir "${MODULE_DIR}" --disc "${ISO}" --headless --vblanks 3
                --events "${events}" --json ${ARGN}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(${prefix}_rc "${rc}" PARENT_SCOPE)
    set(end "")
    if(EXISTS "${events}")
        file(STRINGS "${events}" lines REGEX "\"ev\":\"end\"")
        if(lines)
            list(GET lines -1 end)
        endif()
    endif()
    set(${prefix}_end "${end}" PARENT_SCOPE)
    set(${prefix}_err "${err}" PARENT_SCOPE)
endfunction()

function(expect_digest what json key)
    string(JSON value ERROR_VARIABLE missing GET "${json}" "${key}")
    if(missing OR NOT value MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "${what}: no ${key} in the end event: ${json}")
    endif()
    string(LENGTH "${value}" n)
    if(NOT n EQUAL 40)
        message(FATAL_ERROR "${what}: ${key} is not a SHA-1: ${value}")
    endif()
endfunction()

function(expect_no what json key)
    string(JSON value ERROR_VARIABLE missing GET "${json}" "${key}")
    if(NOT missing)
        message(FATAL_ERROR "${what}: unexpected ${key} = ${value}")
    endif()
endfunction()

run(memory)
if(NOT memory_rc EQUAL 0)
    message(FATAL_ERROR "in-memory run exited ${memory_rc}\n${memory_err}")
endif()
expect_digest("in-memory" "${memory_end}" card_sha1)
expect_no("in-memory" "${memory_end}" card2_sha1)

set(saves "${WORK_DIR}/saves")
run(folder --save-dir "${saves}" --memory-card formatted --memory-card2 unformatted --rtc 2005-11-15T12:00:00)
if(NOT folder_rc EQUAL 0)
    message(FATAL_ERROR "folder run exited ${folder_rc}\n${folder_err}")
endif()
expect_digest("folder" "${folder_end}" card_sha1)
expect_digest("folder" "${folder_end}" card2_sha1)
if(EXISTS "${saves}/dev/card1" OR EXISTS "${saves}/dev/card2")
    message(FATAL_ERROR "a card folder was written although the guest changed nothing")
endif()
run(again --save-dir "${saves}" --memory-card formatted --memory-card2 unformatted --rtc 2005-11-15T12:00:00)
string(JSON a GET "${folder_end}" card_sha1)
string(JSON b GET "${again_end}" card_sha1)
if(NOT a STREQUAL b)
    message(FATAL_ERROR "the same card options gave different digests: ${a} ${b}")
endif()
string(JSON u GET "${folder_end}" card2_sha1)
if(a STREQUAL u)
    message(FATAL_ERROR "a formatted and an unformatted card have the same digest")
endif()

run(none --memory-card none)
if(NOT none_rc EQUAL 0)
    message(FATAL_ERROR "--memory-card none exited ${none_rc}\n${none_err}")
endif()
expect_no("--memory-card none" "${none_end}" card_sha1)

run(badrtc --rtc 2005-02-30T00:00:00)
if(NOT badrtc_rc EQUAL 2)
    message(FATAL_ERROR "--rtc 2005-02-30T00:00:00 exited ${badrtc_rc}, expected 2")
endif()
message(STATUS "ok: card wiring")
