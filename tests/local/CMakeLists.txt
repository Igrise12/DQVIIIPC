# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# Local tests (label "local"): need the user's own disc image, named by DQ8_TEST_ISO. Never run in CI; each
# test skips itself when the variable is unset. Run with: DQ8_TEST_ISO=/path/to.iso ctest --preset local-<cfg>
add_executable(dq8_local_tests
    test_real_disc.cpp
    test_real_overlays.cpp
)
target_compile_definitions(dq8_local_tests PRIVATE DQ8_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(dq8_local_tests PRIVATE dq8::core Catch2::Catch2WithMain)
dq8_set_warnings(dq8_local_tests)
dq8_enable_sanitizers(dq8_local_tests)

include(Catch)
catch_discover_tests(dq8_local_tests PROPERTIES LABELS local)

# The boot to the title screen on the user's disc. Needs the game modules: DQ8_TEST_MODULE_DIR, by default
# work/recomp/modules/SLUS_21207.
#   local.boot_to_title       M3a acceptance 1 as recorded: the M3 input with --movies blank, the movie time
#                             on black as in M3 (the title at vblank 1200).
#   local.boot_to_title_skip  --movies skip, no press after the settings: the flyover timeline is skipped, so the
#                             title comes within 1,600 vblanks (2,640 with the flyover on black).
# The skip run's input is the setup's smoke-test script, kept once in data/smoke/.
dq8_test_program(dq8-game DQ8_TEST_GAME) # tests/unit/CMakeLists.txt: a wrapper when cross compiling
foreach(run IN ITEMS "boot_to_title;tests/local/input/boot-to-title;blank;6000"
                     "boot_to_title_skip;data/smoke/dq8-us-boot-to-title;skip;1600")
    list(GET run 0 name)
    list(GET run 1 script)
    list(GET run 2 movies)
    list(GET run 3 vblanks)
    get_filename_component(script_name "${script}" NAME)
    add_test(NAME local.${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DDQ8_GAME=${DQ8_TEST_GAME}"
            "-DSCRIPT=${PROJECT_SOURCE_DIR}/${script}.dq8in"
            "-DMODULE_DIR=${PROJECT_SOURCE_DIR}/work/recomp/modules/SLUS_21207"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/work"
            "-DNAME=${script_name}"
            "-DMOVIES=${movies}"
            "-DVBLANKS=${vblanks}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_boot_to_title.cmake")
    set_tests_properties(local.${name} PROPERTIES LABELS local TIMEOUT 3600 SKIP_REGULAR_EXPRESSION "SKIPPED:")
endforeach()
