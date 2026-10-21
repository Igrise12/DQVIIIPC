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
# The ABI include root, as cmake/module gives it to generated code. SYSTEM: the macro header is adapted upstream code.
target_include_directories(dq8_unit_contract SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/include/dq8/abi/ps2recomp")
target_compile_features(dq8_unit_contract PRIVATE cxx_std_20)
set_target_properties(dq8_unit_contract PROPERTIES
    PREFIX ""
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
    test_elf_writer.cpp
    test_mips_asm.cpp
    test_process.cpp
    test_function_map.cpp
    test_recomp_config.cpp
    test_dispatch.cpp
    test_guest_memory.cpp
    test_game_module_desc.cpp
    test_ee_runtime_contract.cpp
    test_kernel_calls.cpp
    test_machine_primitives.cpp
    test_kernel_services.cpp
    test_m3_contracts.cpp
    test_fiber.cpp
    test_scheduler.cpp
    test_kernel_threads.cpp
    test_kernel_sync.cpp
    test_kernel_interrupts.cpp
    test_kernel_system.cpp
    test_vu1.cpp
    test_mmio_devices.cpp
    test_dmac.cpp
    test_vif.cpp
    test_gif.cpp
    test_ee_timers.cpp
    test_gs_priv.cpp
    test_gs_trace.cpp
    test_gs_pipeline.cpp
)
target_include_directories(dq8_unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(dq8_unit_tests SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/include/dq8/abi/ps2recomp")
target_compile_definitions(dq8_unit_tests PRIVATE
    DQ8_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    DQ8_CONTRACT_MODULE_DIR="$<TARGET_FILE_DIR:dq8_unit_contract>")
target_link_libraries(dq8_unit_tests PRIVATE dq8::core dq8::runtime dq8::game toml11::toml11 Catch2::Catch2WithMain)
add_dependencies(dq8_unit_tests dq8_unit_contract)
dq8_set_warnings(dq8_unit_tests)
dq8_enable_sanitizers(dq8_unit_tests)

# A portable stand-in program for test_process.cpp: no shell or platform tools needed.
add_executable(dq8_test_child support/process_child.cpp)
dq8_set_warnings(dq8_test_child)
dq8_enable_sanitizers(dq8_test_child)
add_dependencies(dq8_unit_tests dq8_test_child)
target_compile_definitions(dq8_unit_tests PRIVATE DQ8_TEST_CHILD="$<TARGET_FILE:dq8_test_child>")

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

# Without --stop-at-first-syscall the kernel serves SetupThread (M3) and the program loops on it until the
# dispatch budget ends the run: "any other guest stop", exit 1.
add_test(NAME host.dq8_game_elf_contract_exit1
    COMMAND "${CMAKE_COMMAND}"
        "-DDQ8_GAME=$<TARGET_FILE:dq8-game>"
        "-DARGS=--elf@@${_contract_elf}@@--module-dir@@$<TARGET_FILE_DIR:dq8_unit_contract>@@--max-dispatches@@10"
        -DEXPECT_EXIT=1
        "-DEXPECT=stopped after 10 dispatches: max-dispatches: dispatch budget of 10"
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
