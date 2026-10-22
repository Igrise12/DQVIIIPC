# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M5 session A: settings, setup, toolchain, registry and the new dq8ctl commands (test_settings*, test_setup*,
# test_toolchain*, test_registry*, test_cli_*). Add target_sources(dq8_unit_tests PRIVATE ...) here.

# Until the settings store is wired into dq8-game, --config, --set and --status are refused with exit 2 (day 0);
# session A replaces this test with the settings round trip.
add_test(NAME host.dq8_game_m5_options_unwired
    COMMAND "${CMAKE_COMMAND}"
        "-DDQ8_GAME=$<TARGET_FILE:dq8-game>"
        "-DARGS=--elf@@${CMAKE_CURRENT_BINARY_DIR}/contract.elf@@--module-dir@@$<TARGET_FILE_DIR:dq8_unit_contract>@@--set@@display.vsync=false@@--json"
        -DEXPECT_EXIT=2
        "-DEXPECT=\"code\":\"usage\"@@arrive with the settings store"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_and_match.cmake")
set_tests_properties(host.dq8_game_m5_options_unwired PROPERTIES FIXTURES_REQUIRED dq8_contract_elf LABELS unit)
