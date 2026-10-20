# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# One step of one end-to-end fixture, run by CTest as `cmake -D... -P run_e2e.cmake` (see CMakeLists.txt).
#
#   STEP=generate  dq8ctl recomp generate: ps2_recomp on every unit of the fixture
#   STEP=build     dq8ctl recomp build: one game module per unit through cmake/module
#   STEP=exports   every module exports exactly one dynamic symbol, the descriptor entry point
#   STEP=run       dq8-game --elf runs the main unit and must stop at the expected syscall with the expected
#                  a0, v0 and v1
#
# Inputs: FIXTURE_DIR (dq8_e2e_fixtures output for this fixture), WORK_DIR, UNITS (comma-separated, main
# first), DQ8CTL, DQ8_GAME, PS2_RECOMP, NM, MODULE_TEMPLATE (cmake/module), ABI_INCLUDE_DIR, WINDOW
# (BASE:END, overlay fixtures only), RESIDENT (overlay id, overlay fixtures only), EXPECT_SYSCALL,
# EXPECT_A0, EXPECT_V0.
cmake_minimum_required(VERSION 3.25)

# --- Interfaces of sessions B and C that the M2 brief leaves open; checked when they are merged. ----------
# How generate/build receive a unit's ELF without a disc (C).
set(unit_elf_option "--unit-elf")                 # repeated: --unit-elf <unit>=<path>
# Where build finds the module template and the ABI headers from a build tree (C).
set(module_template_option "--module-template")
set(abi_include_option "--abi-include-dir")
# Paths of the stop report in dq8-game's --json output (B), as string(JSON GET) member lists.
set(json_stop_kind stop kind)                     # "syscall"
set(json_syscall_number stop syscall)             # number or "0x..." string
set(json_reg_prefix registers)                    # registers.<name>: number or "0x..." string
# -----------------------------------------------------------------------------------------------------------

foreach(var STEP FIXTURE_DIR WORK_DIR UNITS)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "run_e2e: ${var} is not set")
    endif()
endforeach()
string(REPLACE "," ";" unit_list "${UNITS}")

function(run_checked)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REPLACE ";" " " shown "${ARGN}")
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "exit ${rc}: ${shown}\n--- stdout\n${out}\n--- stderr\n${err}")
    endif()
    message(STATUS "ok: ${shown}")
    set(step_output "${out}" PARENT_SCOPE)
endfunction()

# The game modules the build step produced, wherever the profile directory under modules/ is called.
function(find_modules out_var)
    file(GLOB_RECURSE modules LIST_DIRECTORIES false "${WORK_DIR}/modules/*.so")
    list(LENGTH modules count)
    list(LENGTH unit_list expected)
    if(NOT count EQUAL expected)
        message(FATAL_ERROR "expected ${expected} modules under ${WORK_DIR}/modules, found ${count}: ${modules}")
    endif()
    set(${out_var} "${modules}" PARENT_SCOPE)
endfunction()

# A JSON number or "0x..."/decimal string as a decimal integer.
function(to_number out_var value)
    math(EXPR n "${value}")
    set(${out_var} "${n}" PARENT_SCOPE)
endfunction()

function(expect_equal what actual expected)
    to_number(a "${actual}")
    to_number(e "${expected}")
    if(NOT a EQUAL e)
        math(EXPR ah "${a}" OUTPUT_FORMAT HEXADECIMAL)
        math(EXPR eh "${e}" OUTPUT_FORMAT HEXADECIMAL)
        message(FATAL_ERROR "${what}: got ${ah}, expected ${eh}")
    endif()
    message(STATUS "ok: ${what} = ${actual}")
endfunction()

set(common_args --maps "${FIXTURE_DIR}/maps" --work "${WORK_DIR}" --units "${UNITS}" --json)
set(unit_args "")
foreach(unit IN LISTS unit_list)
    list(APPEND unit_args "${unit_elf_option}" "${unit}=${FIXTURE_DIR}/units/${unit}.elf")
endforeach()

if(STEP STREQUAL "generate")
    file(REMOVE_RECURSE "${WORK_DIR}")
    run_checked("${DQ8CTL}" recomp generate ${common_args} ${unit_args} --ps2-recomp "${PS2_RECOMP}")

elseif(STEP STREQUAL "build")
    run_checked("${DQ8CTL}" recomp build ${common_args} ${unit_args} --jobs 2
                "${module_template_option}" "${MODULE_TEMPLATE}" "${abi_include_option}" "${ABI_INCLUDE_DIR}")
    find_modules(modules)

elseif(STEP STREQUAL "exports")
    find_modules(modules)
    foreach(module IN LISTS modules)
        run_checked("${NM}" -D --defined-only "${module}")
        string(REGEX MATCHALL "[^\n]+" lines "${step_output}")
        list(LENGTH lines count)
        if(NOT count EQUAL 1 OR NOT lines MATCHES " dq8_game_module_describe$")
            message(FATAL_ERROR "${module} must export only dq8_game_module_describe; nm -D shows:\n${step_output}")
        endif()
    endforeach()

elseif(STEP STREQUAL "run")
    find_modules(modules)
    list(GET modules 0 first_module)
    get_filename_component(module_dir "${first_module}" DIRECTORY)
    set(run_args --elf "${FIXTURE_DIR}/units/main.elf" --module-dir "${module_dir}" --stop-at-first-syscall
                 --max-dispatches 1000 --json)
    if(WINDOW)
        list(APPEND run_args --window "${WINDOW}" --resident "${RESIDENT}")
    endif()
    run_checked("${DQ8_GAME}" ${run_args})

    # The report is the last JSON object dq8-game prints.
    string(REGEX MATCHALL "[^\n]+" lines "${step_output}")
    set(report "")
    foreach(line IN LISTS lines)
        string(JSON type ERROR_VARIABLE json_error TYPE "${line}")
        if(NOT json_error AND type STREQUAL "OBJECT")
            set(report "${line}")
        endif()
    endforeach()
    if(report STREQUAL "")
        message(FATAL_ERROR "dq8-game printed no JSON object:\n${step_output}")
    endif()
    string(JSON kind GET "${report}" ${json_stop_kind})
    if(NOT kind STREQUAL "syscall")
        message(FATAL_ERROR "expected a syscall stop, got '${kind}':\n${report}")
    endif()
    string(JSON number GET "${report}" ${json_syscall_number})
    expect_equal("syscall number" "${number}" "${EXPECT_SYSCALL}")
    foreach(reg_expect IN ITEMS "a0=${EXPECT_A0}" "v0=${EXPECT_V0}" "v1=${EXPECT_SYSCALL}")
        string(REPLACE "=" ";" reg_expect "${reg_expect}")
        list(GET reg_expect 0 reg)
        list(GET reg_expect 1 value)
        string(JSON actual GET "${report}" ${json_reg_prefix} ${reg})
        expect_equal("${reg}" "${actual}" "${value}")
    endforeach()

else()
    message(FATAL_ERROR "run_e2e: unknown STEP '${STEP}'")
endif()
