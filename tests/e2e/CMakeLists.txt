# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# End-to-end tests (label "e2e", DQ8_BUILD_E2E): hand-assembled MIPS code units go through the whole pipeline,
# ps2_recomp -> game module -> dq8-game, and must stop at the expected syscall with the expected registers.
# No game data: fixture_gen.cpp assembles every word. See README.md.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "DQ8_BUILD_E2E is Linux-only for now (the exports check uses nm -D on .so modules)")
endif()

add_executable(dq8_e2e_fixtures fixture_gen.cpp)
target_include_directories(dq8_e2e_fixtures PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
target_link_libraries(dq8_e2e_fixtures PRIVATE dq8::core)
dq8_set_warnings(dq8_e2e_fixtures)
dq8_enable_sanitizers(dq8_e2e_fixtures)

set(_e2e_work "${CMAKE_CURRENT_BINARY_DIR}/work")
add_test(NAME e2e.fixtures COMMAND dq8_e2e_fixtures "${_e2e_work}/fixtures")
set_tests_properties(e2e.fixtures PROPERTIES LABELS e2e FIXTURES_SETUP e2e_fixtures)

# The pipeline's programs. dq8-game (session B) and cmake/module (session A) arrive with M2; until both exist
# only the fixture test is registered.
set(_e2e_template "${PROJECT_SOURCE_DIR}/cmake/module")
if(NOT TARGET dq8-game OR NOT EXISTS "${_e2e_template}/CMakeLists.txt")
    message(WARNING "e2e: dq8-game or cmake/module is missing; only e2e.fixtures is registered")
    return()
endif()
find_program(DQ8_NM NAMES nm REQUIRED)

# fixture  units     window                 resident  run: syscall (M2 single thread) or machine (M3)
set(_e2e_fixtures
    "call|main|||syscall"
    "bgezal|main|||syscall"
    "overlay|main,ovl|0x00300000:0x00310000|1|syscall"
    "irq|main|||machine")
foreach(_row IN LISTS _e2e_fixtures)
    string(REPLACE "|" ";" _row "${_row}")
    list(GET _row 0 _fixture)
    list(GET _row 1 _units)
    list(GET _row 2 _window)
    list(GET _row 3 _resident)
    list(GET _row 4 _run_mode)
    set(_previous e2e_fixtures)
    foreach(_step generate build exports run)
        set(_name e2e.${_fixture}.${_step})
        add_test(NAME ${_name}
                 COMMAND "${CMAKE_COMMAND}"
                         -DSTEP=${_step}
                         "-DFIXTURE_DIR=${_e2e_work}/fixtures/${_fixture}"
                         "-DWORK_DIR=${_e2e_work}/${_fixture}"
                         -DUNITS=${_units}
                         "-DDQ8CTL=$<TARGET_FILE:dq8ctl>"
                         "-DDQ8_GAME=$<TARGET_FILE:dq8-game>"
                         "-DPS2_RECOMP=$<TARGET_FILE:dq8::ps2_recomp>"
                         "-DNM=${DQ8_NM}"
                         "-DMODULE_TEMPLATE=${_e2e_template}"
                         "-DABI_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include"
                         "-DWINDOW=${_window}"
                         "-DRESIDENT=${_resident}"
                         -DRUN_MODE=${_run_mode}
                         -DEXPECT_SYSCALL=0x3C
                         -DEXPECT_A0=0x12345678
                         -DEXPECT_V0=0x12345679
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/run_e2e.cmake")
        # Each step needs the previous one; exports and run both need build.
        set_tests_properties(${_name} PROPERTIES LABELS e2e TIMEOUT 900 FIXTURES_REQUIRED ${_previous})
        if(_step STREQUAL "generate" OR _step STREQUAL "build")
            set_tests_properties(${_name} PROPERTIES FIXTURES_SETUP ${_name})
            set(_previous ${_name})
        endif()
    endforeach()
endforeach()
