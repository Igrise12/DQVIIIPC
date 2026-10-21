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

# M3a acceptance 1: the boot to the title screen on the user's disc (needs the M3 machine in dq8-game and the
# game modules: DQ8_TEST_MODULE_DIR, default work/recomp/modules/SLUS_21207).
if(DQ8_HOST_MACHINE)
    add_test(NAME local.boot_to_title
        COMMAND "${CMAKE_COMMAND}"
            "-DDQ8_GAME=$<TARGET_FILE:dq8-game>"
            "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/input/boot-to-title.dq8in"
            "-DMODULE_DIR=${PROJECT_SOURCE_DIR}/work/recomp/modules/SLUS_21207"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/work"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_boot_to_title.cmake")
    set_tests_properties(local.boot_to_title PROPERTIES LABELS local TIMEOUT 3600 SKIP_REGULAR_EXPRESSION "SKIPPED:")
endif()
