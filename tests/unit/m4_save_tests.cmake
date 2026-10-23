# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M4 session D: memory card, RTC, pad feedback, VU0 and kernel tests (test_memcard*, ...).
target_sources(dq8_unit_tests PRIVATE
    test_memcard.cpp
    test_pad_feedback.cpp
    test_vu0_registers.cpp
    test_kernel_m4.cpp
    # wire_feedback is linked into dq8-game only; its test builds it here.
    ${PROJECT_SOURCE_DIR}/src/host/feedback_wiring.cpp
)
# dq8-game's card options on the synthetic machine program (the inputs come from the host.machine_inputs
# fixture of CMakeLists.txt).
add_test(NAME host.dq8_game_card_wiring
    COMMAND "${CMAKE_COMMAND}"
        "-DDQ8_GAME=${DQ8_TEST_GAME}"
        "-DELF=${CMAKE_CURRENT_BINARY_DIR}/machine.elf"
        "-DMODULE_DIR=$<TARGET_FILE_DIR:dq8_unit_machine>"
        "-DISO=${CMAKE_CURRENT_BINARY_DIR}/machine.iso"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/card_wiring"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_card_wiring.cmake")
set_tests_properties(host.dq8_game_card_wiring PROPERTIES FIXTURES_REQUIRED dq8_machine_inputs LABELS unit)
