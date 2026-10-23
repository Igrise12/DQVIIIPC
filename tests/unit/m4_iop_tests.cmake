# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M4 session A: the interpreted IOP's unit tests (test_iop_*, test_irx_*).
target_sources(dq8_unit_tests PRIVATE
    support/irx_builder.cpp
    test_iop_cpu.cpp
    test_irx_loader.cpp
    test_iop_kernel.cpp
    test_iop_sif.cpp
    test_iop_hw.cpp
)

# --dump-iop-ram-at on the synthetic machine run (the fixture of tests/unit/CMakeLists.txt): the dump is written
# and reported as an event with its SHA-1.
add_test(NAME host.dq8_game_dump_iop_ram
    COMMAND "${CMAKE_COMMAND}"
        "-DDQ8_GAME=${DQ8_TEST_GAME}"
        "-DARGS=--elf@@${CMAKE_CURRENT_BINARY_DIR}/machine.elf@@--module-dir@@$<TARGET_FILE_DIR:dq8_unit_machine>@@--disc@@${CMAKE_CURRENT_BINARY_DIR}/machine.iso@@--headless@@--gs@@null@@--vblanks@@3@@--dump-iop-ram-at@@2:${CMAKE_CURRENT_BINARY_DIR}/machine.iopram@@--events@@-"
        -DEXPECT_EXIT=0
        "-DEXPECT=\"ev\":\"iop_ram_dump\",\"vblank\":2,\"cycles\":[0-9]+,\"sha1\":\"[0-9a-f]+\""
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_and_match.cmake")
set_tests_properties(host.dq8_game_dump_iop_ram PROPERTIES FIXTURES_REQUIRED dq8_machine_inputs LABELS unit)
