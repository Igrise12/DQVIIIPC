# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M5 session A's end-to-end checks of dq8-game and dq8ctl (m5_core_tests.cmake), one MODE per test:
#   default_config  the synthetic machine run gives byte-identical --events with and without a settings file
#                   holding every default (`dq8ctl settings defaults`): default settings reproduce M4
#   status          --status FILE over a 1.5 s run at --speed 1: started, first_frame, fps, a warning for a bad
#                   value in --config, exit with reason "vblanks"
#   status_stdout   --status -: stdout carries only status lines; the --json report moves to stderr
#   settings_cli    dq8ctl settings set/get/reset/list/defaults on a file with unknown keys and a profile table
#   cmake -DMODE=<mode> -DDQ8_GAME=<exe> -DDQ8CTL=<exe> -DELF=<elf> -DMODULE_DIR=<dir> -DISO=<iso>
#         -DWORK_DIR=<dir> -P run_m5_core.cmake
cmake_minimum_required(VERSION 3.25)

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

function(run_ok out_var)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "exit ${rc}: ${ARGN}\n--- stdout\n${out}\n--- stderr\n${err}")
    endif()
    set(${out_var} "${out}" PARENT_SCOPE)
    set(${out_var}_err "${err}" PARENT_SCOPE)
endfunction()

function(expect_match what text pattern)
    if(NOT text MATCHES "${pattern}")
        message(FATAL_ERROR "${what}: no match for '${pattern}' in\n${text}")
    endif()
    message(STATUS "ok: ${what}")
endfunction()

set(machine --elf "${ELF}" --module-dir "${MODULE_DIR}" --disc "${ISO}" --headless --gs trace)

if(MODE STREQUAL "default_config")
    run_ok(wrote "${DQ8CTL}" settings defaults --config "${WORK_DIR}/defaults.toml")
    file(READ "${WORK_DIR}/defaults.toml" defaults)
    expect_match("the defaults file holds every setting" "${defaults}" "window_mode = \"windowed\".*memory_card2 = \"none\"")
    run_ok(plain "${DQ8_GAME}" ${machine} --vblanks 10 --events "${WORK_DIR}/m4.jsonl" --json)
    run_ok(configured "${DQ8_GAME}" ${machine} --vblanks 10 --events "${WORK_DIR}/default.jsonl" --json
           --config "${WORK_DIR}/defaults.toml")
    expect_match("the defaults applied" "${configured}" "\"settings\":\\[\"audio.output=sdl\",\"audio.latency_ms=64\"")
    file(SHA1 "${WORK_DIR}/m4.jsonl" a)
    file(SHA1 "${WORK_DIR}/default.jsonl" b)
    if(NOT a STREQUAL b)
        message(FATAL_ERROR "the event streams differ with the default settings file (${a} against ${b})")
    endif()
    message(STATUS "ok: identical event streams (${a})")
elseif(MODE STREQUAL "status")
    file(WRITE "${WORK_DIR}/settings.toml" "schema_version = 1\n[audio]\noutput = \"null\"\nlatency_ms = 5\n")
    run_ok(out "${DQ8_GAME}" ${machine} --vblanks 90 --speed 1 --config "${WORK_DIR}/settings.toml"
           --status "${WORK_DIR}/status.jsonl")
    file(STRINGS "${WORK_DIR}/status.jsonl" lines)
    list(GET lines 0 first)
    list(GET lines -1 last)
    expect_match("started first" "${first}" "^{\"ev\":\"started\",\"version\":\"[^\"]+\",\"profile\":\"dev\",\"pid\":[0-9]+,\"window\":false}$")
    file(READ "${WORK_DIR}/status.jsonl" all)
    expect_match("first_frame" "${all}" "{\"ev\":\"first_frame\",\"ms\":[0-9]+}")
    expect_match("fps" "${all}" "{\"ev\":\"fps\",\"fps\":[0-9.]+,\"speed\":[0-9.]+}")
    expect_match("the bad value as a warning" "${all}" "{\"ev\":\"warning\",\"code\":\"settings-invalid\",\"message\":\"[^\"]*audio.latency_ms = 5")
    expect_match("exit last" "${last}" "^{\"ev\":\"exit\",\"status\":0,\"reason\":\"vblanks\"}$")
elseif(MODE STREQUAL "status_stdout")
    run_ok(out "${DQ8_GAME}" ${machine} --vblanks 3 --status - --json)
    string(STRIP "${out}" out)
    string(REPLACE "\n" ";" lines "${out}")
    foreach(line IN LISTS lines)
        if(NOT line MATCHES "^{\"ev\":\"(started|first_frame|fps|warning|exit)\"")
            message(FATAL_ERROR "stdout carries something other than status lines: '${line}'")
        endif()
    endforeach()
    expect_match("the report went to stderr" "${out_err}" "\"ok\":true")
    expect_match("exit last" "${out}" "{\"ev\":\"exit\",\"status\":0,\"reason\":\"vblanks\"}$")
elseif(MODE STREQUAL "settings_cli")
    set(file "${WORK_DIR}/settings.toml")
    file(WRITE "${file}" "# mine\nschema_version = 1\nlater = \"kept\"\n[display]\nshape = \"round\"\n")
    run_ok(o "${DQ8CTL}" settings set display.width=1600 audio.volume=80 --config "${file}")
    run_ok(o "${DQ8CTL}" settings set game.movies skip --profile SLUS_21207 --config "${file}")
    run_ok(width "${DQ8CTL}" settings get display.width --config "${file}")
    expect_match("get" "${width}" "^1600\n$")
    run_ok(movies "${DQ8CTL}" settings get game.movies --config "${file}" --profile SLUS_21207 --json)
    expect_match("the profile's value" "${movies}" "\"value\":\"skip\",\"source\":\"profile\"")
    file(READ "${file}" text)
    expect_match("unknown keys and comments kept" "${text}" "# mine.*later = \"kept\".*shape = \"round\".*width = 1600")
    expect_match("the profile table" "${text}" "\\[profile\\.SLUS_21207\\.game\\]\nmovies = \"skip\"")
    run_ok(o "${DQ8CTL}" settings reset display.width --config "${file}")
    run_ok(width "${DQ8CTL}" settings get display.width --config "${file}")
    expect_match("reset" "${width}" "^1280\n$")
    run_ok(list "${DQ8CTL}" settings list --config "${file}" --json)
    expect_match("list" "${list}" "{\"key\":\"audio.volume\",\"value\":80,\"source\":\"user\",\"type\":\"integer\"")
    execute_process(COMMAND "${DQ8CTL}" settings set display.width=1 --config "${file}" --json
                    RESULT_VARIABLE rc OUTPUT_VARIABLE bad ERROR_VARIABLE bad_err)
    if(NOT rc EQUAL 2)
        message(FATAL_ERROR "a bad value exited ${rc}, expected 2")
    endif()
    expect_match("a bad value is refused" "${bad}" "\"code\":\"settings-invalid\"")
else()
    message(FATAL_ERROR "unknown MODE '${MODE}'")
endif()
