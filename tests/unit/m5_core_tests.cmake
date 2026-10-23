# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M5 session A: settings, setup, toolchain, registry and the new dq8ctl commands (test_settings*, test_setup*,
# test_toolchain*, test_registry*, test_host_settings*, and the end-to-end runs below).
target_sources(dq8_unit_tests PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/test_settings.cpp
    ${CMAKE_CURRENT_LIST_DIR}/test_registry.cpp
    ${CMAKE_CURRENT_LIST_DIR}/test_toolchain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/test_setup.cpp
    ${CMAKE_CURRENT_LIST_DIR}/test_host_settings.cpp
)

# dq8-game --config and --set over the contract unit: every M5 session A row applies, the report lists them.
set(_m5a "${CMAKE_CURRENT_BINARY_DIR}/m5_core")
file(MAKE_DIRECTORY "${_m5a}")
file(WRITE "${_m5a}/settings-a.toml"
    "schema_version = 1\n[audio]\noutput = \"null\"\nlatency_ms = 200\n[input]\nrumble = false\n"
    "[game]\nmovies = \"skip\"\nsave_dir = \"${_m5a}/saves\"\nmemory_card2 = \"unformatted\"\n")
file(WRITE "${_m5a}/damaged.toml" "[audio\n")
set(_contract_run "--elf@@${CMAKE_CURRENT_BINARY_DIR}/contract.elf@@--module-dir@@$<TARGET_FILE_DIR:dq8_unit_contract>")
add_test(NAME host.dq8_game_settings_applied
    COMMAND "${CMAKE_COMMAND}"
        "-DDQ8_GAME=$<TARGET_FILE:dq8-game>"
        "-DARGS=${_contract_run}@@--config@@${_m5a}/settings-a.toml@@--set@@game.memory_card2=formatted@@--stop-at-first-syscall@@--json"
        -DEXPECT_EXIT=0
        "-DEXPECT=\"settings\":\\[\"display.window_mode=windowed\",\"display.width=1280\",\"display.height=960\",\"display.vsync=true\",\"display.aspect=auto\",\"display.integer_scaling=false\",\"display.filter=linear\",\"audio.output=null\",\"audio.device=\",\"audio.volume=100\",\"audio.latency_ms=200\",\"input.profile=default\",\"input.deadzone=8\",\"input.rumble=false\",\"game.movies=skip\",\"game.save_dir=[^\"]+/saves\",\"game.memory_card2=formatted\"\\]"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_and_match.cmake")
# A flag on the command line wins over the file: --movies play leaves game.movies out.
add_test(NAME host.dq8_game_settings_flag_wins
    COMMAND "${CMAKE_COMMAND}"
        "-DDQ8_GAME=$<TARGET_FILE:dq8-game>"
        "-DARGS=${_contract_run}@@--config@@${_m5a}/settings-a.toml@@--movies@@play@@--stop-at-first-syscall@@--json"
        -DEXPECT_EXIT=0
        "-DEXPECT=\"input.rumble=false\",\"game.save_dir="
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_and_match.cmake")
add_test(NAME host.dq8_game_settings_bad_set
    COMMAND "${CMAKE_COMMAND}"
        "-DDQ8_GAME=$<TARGET_FILE:dq8-game>"
        "-DARGS=${_contract_run}@@--set@@display.width=1@@--json"
        -DEXPECT_EXIT=2
        "-DEXPECT=\"code\":\"settings-invalid\"@@display.width = '1' is outside 640 to 7680"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_and_match.cmake")
add_test(NAME host.dq8_game_settings_damaged
    COMMAND "${CMAKE_COMMAND}"
        "-DDQ8_GAME=$<TARGET_FILE:dq8-game>"
        "-DARGS=${_contract_run}@@--config@@${_m5a}/damaged.toml@@--json"
        -DEXPECT_EXIT=2
        "-DEXPECT=\"code\":\"settings-parse\"@@damaged.toml:1:"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_and_match.cmake")
set_tests_properties(host.dq8_game_settings_applied host.dq8_game_settings_flag_wins host.dq8_game_settings_bad_set
                     host.dq8_game_settings_damaged PROPERTIES FIXTURES_REQUIRED dq8_contract_elf LABELS unit)

# The synthetic machine run with settings and the status channel, and dq8ctl settings (run_m5_core.cmake).
foreach(mode IN ITEMS default_config status status_stdout settings_cli)
    add_test(NAME m5_core.${mode}
        COMMAND "${CMAKE_COMMAND}"
            -DMODE=${mode}
            "-DDQ8_GAME=$<TARGET_FILE:dq8-game>"
            "-DDQ8CTL=$<TARGET_FILE:dq8ctl>"
            "-DELF=${CMAKE_CURRENT_BINARY_DIR}/machine.elf"
            "-DMODULE_DIR=$<TARGET_FILE_DIR:dq8_unit_machine>"
            "-DISO=${CMAKE_CURRENT_BINARY_DIR}/machine.iso"
            "-DWORK_DIR=${_m5a}/${mode}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_m5_core.cmake")
    # The default file asks for SDL audio; its dummy driver needs no device (the sink is after the hashes).
    set_tests_properties(m5_core.${mode} PROPERTIES FIXTURES_REQUIRED dq8_machine_inputs LABELS unit
                         ENVIRONMENT "SDL_AUDIO_DRIVER=dummy")
endforeach()

# dq8ctl's M5 commands without a disc: the setup's events for an image that is not there, diagnose, and run
# with nothing registered.
add_test(NAME cli.dq8ctl_setup_events
    COMMAND "${CMAKE_COMMAND}"
        "-DDQ8_GAME=$<TARGET_FILE:dq8ctl>"
        "-DARGS=setup@@${_m5a}/missing.iso@@--events@@-@@--work@@${_m5a}/setup-work@@--registry@@${_m5a}/installs.toml"
        -DEXPECT_EXIT=2
        "-DEXPECT=^{\"ev\":\"stage\",\"stage\":\"verify\",\"state\":\"start\"@@{\"ev\":\"stage\",\"stage\":\"verify\",\"state\":\"error\",[^\n]*\"code\":\"io-open\"}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_and_match.cmake")
add_test(NAME cli.dq8ctl_diagnose
    COMMAND "${CMAKE_COMMAND}"
        "-DDQ8_GAME=$<TARGET_FILE:dq8ctl>"
        "-DARGS=diagnose@@--registry@@${_m5a}/no-installs.toml@@--work@@${_m5a}/no-work@@--config@@${_m5a}/settings-a.toml"
        -DEXPECT_EXIT=0
        "-DEXPECT=\\[system\\]@@\\[toolchain\\]@@game.movies = skip \\(user\\)@@no game is set up yet@@no setup log"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_and_match.cmake")
add_test(NAME cli.dq8ctl_run_nothing_registered
    COMMAND "${CMAKE_COMMAND}"
        "-DDQ8_GAME=$<TARGET_FILE:dq8ctl>"
        "-DARGS=run@@--registry@@${_m5a}/no-installs.toml@@--json"
        -DEXPECT_EXIT=2
        "-DEXPECT=no game is set up"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_and_match.cmake")
set_tests_properties(cli.dq8ctl_setup_events cli.dq8ctl_diagnose cli.dq8ctl_run_nothing_registered PROPERTIES
                     LABELS unit)
