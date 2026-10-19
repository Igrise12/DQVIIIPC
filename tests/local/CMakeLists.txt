# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# Local tests (label "local"): need the user's own disc image, named by DQ8_TEST_ISO. Never run in CI; each
# test skips itself when the variable is unset. Run with: DQ8_TEST_ISO=/path/to.iso ctest --preset local-<cfg>
add_executable(dq8_local_tests
    test_real_disc.cpp
)
target_compile_definitions(dq8_local_tests PRIVATE DQ8_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(dq8_local_tests PRIVATE dq8::core Catch2::Catch2WithMain)
dq8_set_warnings(dq8_local_tests)
dq8_enable_sanitizers(dq8_local_tests)

include(Catch)
catch_discover_tests(dq8_local_tests PROPERTIES LABELS local)
