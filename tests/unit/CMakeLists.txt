# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# Unit tests: no game data, run in CI (label "unit"). Tests that need the user's disc carry the label "local"
# and live under tests/local/.

# The hand-written contract unit (support/contract_unit.cpp) as a real game module, the way cmake/module builds
# one: a shared library with hidden visibility whose only export is dq8_game_module_describe. The unit tests
# load it with dlopen/LoadLibrary, and dq8-game --elf runs it below.
add_library(dq8_unit_contract SHARED
    support/contract_unit.cpp
    support/contract_unit_module.cpp
)
target_include_directories(dq8_unit_contract PRIVATE "${PROJECT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(dq8_unit_contract PRIVATE cxx_std_20)
set_target_properties(dq8_unit_contract PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/contract_modules"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/contract_modules")
dq8_set_warnings(dq8_unit_contract)
dq8_enable_sanitizers(dq8_unit_contract)

add_executable(dq8_unit_tests
    support/iso_builder.cpp
    support/mwo3_builder.cpp
    support/contract_unit.cpp
    test_version.cpp
    test_hash.cpp
    test_iso9660.cpp
    test_elf_headers.cpp
    test_mwo3.cpp
    test_region_profile.cpp
    test_intake.cpp
    test_dispatch.cpp
    test_guest_memory.cpp
    test_game_module_desc.cpp
    test_ee_runtime_contract.cpp
    test_kernel_calls.cpp
)
target_include_directories(dq8_unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(dq8_unit_tests PRIVATE
    DQ8_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    DQ8_CONTRACT_MODULE_DIR="$<TARGET_FILE_DIR:dq8_unit_contract>")
target_link_libraries(dq8_unit_tests PRIVATE dq8::core dq8::runtime dq8::game Catch2::Catch2WithMain)
add_dependencies(dq8_unit_tests dq8_unit_contract)
dq8_set_warnings(dq8_unit_tests)
dq8_enable_sanitizers(dq8_unit_tests)

include(Catch)
catch_discover_tests(dq8_unit_tests PROPERTIES LABELS unit)

# dq8-game --elf end to end on the contract unit: an ELF for it, then a run that must stop at its first syscall.
add_executable(dq8_contract_elf support/write_contract_elf.cpp)
target_include_directories(dq8_contract_elf PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(dq8_contract_elf PRIVATE dq8::core)
dq8_set_warnings(dq8_contract_elf)

set(_contract_elf "${CMAKE_CURRENT_BINARY_DIR}/contract.elf")
add_test(NAME host.contract_elf COMMAND dq8_contract_elf "${_contract_elf}")
set_tests_properties(host.contract_elf PROPERTIES FIXTURES_SETUP dq8_contract_elf LABELS unit)

add_test(NAME host.dq8_game_elf_contract
    COMMAND "${CMAKE_COMMAND}"
        "-DDQ8_GAME=$<TARGET_FILE:dq8-game>"
        "-DARGS=--elf@@${_contract_elf}@@--module-dir@@$<TARGET_FILE_DIR:dq8_unit_contract>@@--stop-at-first-syscall@@--json"
        -DEXPECT_EXIT=0
        "-DEXPECT=\"reason\":\"syscall\"@@\"number\":\"0x3C\"@@\"name\":\"SetupThread\"@@\"a0\":\"0x12345678\"@@\"v0\":\"0x0000000012345679\""
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_and_match.cmake")
set_tests_properties(host.dq8_game_elf_contract PROPERTIES FIXTURES_REQUIRED dq8_contract_elf LABELS unit)

# Without --stop-at-first-syscall the same stop is "any other guest stop": exit 1.
add_test(NAME host.dq8_game_elf_contract_exit1
    COMMAND "${CMAKE_COMMAND}"
        "-DDQ8_GAME=$<TARGET_FILE:dq8-game>"
        "-DARGS=--elf@@${_contract_elf}@@--module-dir@@$<TARGET_FILE_DIR:dq8_unit_contract>"
        -DEXPECT_EXIT=1
        "-DEXPECT=stopped after 2 dispatches: syscall: syscall 0x3C [(]SetupThread[)]"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_and_match.cmake")
set_tests_properties(host.dq8_game_elf_contract_exit1 PROPERTIES FIXTURES_REQUIRED dq8_contract_elf LABELS unit)

# A module directory with no modules is a validation error: exit 2 with the problem listed.
add_test(NAME host.dq8_game_elf_no_modules
    COMMAND "${CMAKE_COMMAND}"
        "-DDQ8_GAME=$<TARGET_FILE:dq8-game>"
        "-DARGS=--elf@@${_contract_elf}@@--module-dir@@${CMAKE_CURRENT_SOURCE_DIR}/support@@--json"
        -DEXPECT_EXIT=2
        "-DEXPECT=\"code\":\"module-validation\"@@no unit modules"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_and_match.cmake")
set_tests_properties(host.dq8_game_elf_no_modules PROPERTIES FIXTURES_REQUIRED dq8_contract_elf LABELS unit)
