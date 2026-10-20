# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# ABI tests (label "unit", names "abi.*"; no game data): the game-module ABI headers compile on their own under the
# project's warnings; ps2_call_list.h is still upstream's; the macros route and forward as documented; and, with
# DQ8_BUILD_ABI_MODULE_TEST, a sample unit in the recompiler's shape builds through cmake/module into a module that
# loads, describes itself and runs. See README.md.

# Declared here rather than in cmake/Dq8Options.cmake, which another M2 session owns; move it there when merging.
if(WIN32)
    set(_dq8_abi_module_test_default OFF) # the loader test is POSIX-only; the template's MSVC path is unverified
else()
    set(_dq8_abi_module_test_default ON)
endif()
option(DQ8_BUILD_ABI_MODULE_TEST "Build tests/abi/sample_unit through cmake/module and test loading it"
       ${_dq8_abi_module_test_default})

set(_abi_include "${PROJECT_SOURCE_DIR}/include")
set(_abi_ps2recomp "${PROJECT_SOURCE_DIR}/include/dq8/abi/ps2recomp")

# Each ABI header alone in a translation unit, built with the project's warnings and no -msse4.1.
set(_standalone_sources "")
foreach(_header game_module.h ps2_address.h r5900_context.h ps2_runtime.h ps2_runtime_macros.h ps2_call_list.h
                ps2_named_call.h ps2_syscalls.h ps2_stubs.h ps2_log.h)
    string(MAKE_C_IDENTIFIER "${_header}" _id)
    set(_source "${CMAKE_CURRENT_BINARY_DIR}/standalone/${_id}.cpp")
    if(_header STREQUAL "game_module.h")
        file(CONFIGURE OUTPUT "${_source}" CONTENT "#include \"dq8/abi/game_module.h\"\n")
    else()
        file(CONFIGURE OUTPUT "${_source}" CONTENT "#include \"${_header}\"\n")
    endif()
    list(APPEND _standalone_sources "${_source}")
endforeach()
add_library(dq8_abi_standalone_headers OBJECT ${_standalone_sources})
target_include_directories(dq8_abi_standalone_headers PRIVATE "${_abi_include}" "${_abi_ps2recomp}")
target_compile_features(dq8_abi_standalone_headers PRIVATE cxx_std_20)
dq8_set_warnings(dq8_abi_standalone_headers)

add_test(NAME abi.call_list_parity
         COMMAND "${CMAKE_COMMAND}" "-DOURS=${_abi_ps2recomp}/ps2_call_list.h"
                 "-DUPSTREAM=${PROJECT_SOURCE_DIR}/third_party/PS2Recomp/ps2xRuntime/include/ps2_call_list.h"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/check_call_list_parity.cmake")
set_tests_properties(abi.call_list_parity PROPERTIES LABELS unit)

add_executable(dq8_abi_tests test_macros.cpp)
target_include_directories(dq8_abi_tests PRIVATE "${_abi_include}" "${_abi_ps2recomp}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(dq8_abi_tests PRIVATE cxx_std_20)
target_link_libraries(dq8_abi_tests PRIVATE Catch2::Catch2WithMain)
if(NOT MSVC)
    target_compile_options(dq8_abi_tests PRIVATE -msse4.1) # as game modules are built
endif()
dq8_set_warnings(dq8_abi_tests)
dq8_enable_sanitizers(dq8_abi_tests)

if(DQ8_BUILD_ABI_MODULE_TEST)
    include(ExternalProject)
    set(_sample_build "${CMAKE_CURRENT_BINARY_DIR}/sample_module")
    set(_sample_module "${_sample_build}/module/dq8_unit_sample${CMAKE_SHARED_MODULE_SUFFIX}")
    set(_sample_args
        "-DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}"
        "-DCMAKE_BUILD_TYPE:STRING=Release"
        "-DDQ8_UNIT_NAME:STRING=sample"
        "-DDQ8_UNIT_KIND:STRING=main"
        "-DDQ8_UNIT_OVERLAY_ID:STRING=0"
        "-DDQ8_UNIT_GENERATED_DIR:PATH=${CMAKE_CURRENT_SOURCE_DIR}/sample_unit"
        "-DDQ8_UNIT_PROFILE_ID:STRING=SAMPLE"
        "-DDQ8_UNIT_SOURCE_SHA1:STRING=0123456789abcdef0123456789abcdef01234567"
        "-DDQ8_GENERATOR:STRING=hand-written sample"
        "-DDQ8_UNIT_ENTRY_POINT:STRING=0x200000"
        "-DDQ8_ABI_INCLUDE_DIR:PATH=${_abi_include}")
    if(CMAKE_CXX_COMPILER_LAUNCHER)
        list(APPEND _sample_args "-DCMAKE_CXX_COMPILER_LAUNCHER:STRING=${CMAKE_CXX_COMPILER_LAUNCHER}")
    endif()
    ExternalProject_Add(dq8_abi_sample_module
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/cmake/module"
        BINARY_DIR "${_sample_build}"
        CMAKE_CACHE_ARGS ${_sample_args}
        BUILD_ALWAYS ON
        BUILD_BYPRODUCTS "${_sample_module}"
        INSTALL_COMMAND ""
        TEST_COMMAND "")

    target_sources(dq8_abi_tests PRIVATE test_module_load.cpp)
    target_compile_definitions(dq8_abi_tests PRIVATE DQ8_ABI_SAMPLE_MODULE="${_sample_module}")
    target_link_libraries(dq8_abi_tests PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(dq8_abi_tests dq8_abi_sample_module)

    # Exactly one exported symbol (-z defs already refused any undefined reference at link time).
    if(CMAKE_NM)
        add_test(NAME abi.sample_module_exports
                 COMMAND "${CMAKE_COMMAND}" "-DNM=${CMAKE_NM}" "-DMODULE=${_sample_module}"
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/check_module_exports.cmake")
        set_tests_properties(abi.sample_module_exports PROPERTIES LABELS unit)
    endif()
endif()

include(Catch)
catch_discover_tests(dq8_abi_tests TEST_PREFIX "abi." PROPERTIES LABELS unit)
