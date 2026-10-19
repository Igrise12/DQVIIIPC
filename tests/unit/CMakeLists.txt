# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# Unit tests: no game data, run in CI (label "unit"). Tests that need the user's disc carry the label "local"
# and live under tests/local/ (added with M1).
add_executable(dq8_unit_tests
    test_version.cpp
)
target_link_libraries(dq8_unit_tests PRIVATE dq8::core Catch2::Catch2WithMain)
dq8_set_warnings(dq8_unit_tests)
dq8_enable_sanitizers(dq8_unit_tests)

include(Catch)
catch_discover_tests(dq8_unit_tests PROPERTIES LABELS unit)
