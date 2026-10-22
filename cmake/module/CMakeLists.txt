# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#
# Builds one game module: the recompiled C++ of one code unit (the main program or one overlay) as a shared
# library that exports only dq8_game_module_describe (include/dq8/abi/game_module.h). A standalone project, one
# build tree per unit, installed with the host and driven by `dq8ctl recomp build`:
#
#   cmake -S <this dir> -B <work>/build/<unit> -G Ninja -DDQ8_UNIT_NAME=main -DDQ8_UNIT_KIND=main ...
#   cmake --build <work>/build/<unit> -j N
#
# Output: <build>/module/dq8_unit_<name><suffix> (dq8_unit_main.so, dq8_unit_main.dll); `cmake --install <build>
# --prefix <dir>` copies it to <dir>. Docs/milestones/M2-first-recompile.md has the design.
cmake_minimum_required(VERSION 3.25)
project(dq8_game_module LANGUAGES CXX)

set(DQ8_UNIT_NAME "" CACHE STRING "Code unit name: main, title, battle, ... ([a-z0-9_]+)")
set(DQ8_UNIT_KIND "" CACHE STRING "main or overlay")
set(DQ8_UNIT_OVERLAY_ID "0" CACHE STRING "The MWo3 header's overlay_id; 0 for the main unit")
set(DQ8_UNIT_GENERATED_DIR "" CACHE PATH "Directory holding the recompiler's output for this unit")
set(DQ8_UNIT_PROFILE_ID "" CACHE STRING "Region profile id, e.g. SLUS_21207")
set(DQ8_UNIT_SOURCE_SHA1 "" CACHE STRING "SHA-1 of the boot ELF or the overlay container (40 hex digits)")
set(DQ8_GENERATOR "" CACHE STRING "Recompiler identity, e.g. \"ps2recomp eb8688b2\"")
set(DQ8_UNIT_ENTRY_POINT "0" CACHE STRING "Guest entry point of the main unit (0x... or decimal); 0 for overlays")
set(DQ8_ABI_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../include" CACHE PATH
    "Directory that holds dq8/abi/game_module.h and dq8/abi/ps2recomp/")
option(DQ8_MODULE_PCH "Precompile the ABI headers and the unit's function headers" ON)
option(DQ8_MODULE_DEBUG_INFO "Emit debug information" OFF)

# ---------------------------------------------------------------------------------------------------------------
# Parameters
# ---------------------------------------------------------------------------------------------------------------
set(_problems "")
if(NOT DQ8_UNIT_NAME MATCHES "^[a-z0-9_]+$")
    list(APPEND _problems "DQ8_UNIT_NAME must match [a-z0-9_]+ (got '${DQ8_UNIT_NAME}')")
endif()
if(NOT DQ8_UNIT_KIND MATCHES "^(main|overlay)$")
    list(APPEND _problems "DQ8_UNIT_KIND must be main or overlay (got '${DQ8_UNIT_KIND}')")
endif()
if(NOT DQ8_UNIT_OVERLAY_ID MATCHES "^[0-9]+$")
    list(APPEND _problems "DQ8_UNIT_OVERLAY_ID must be a decimal number (got '${DQ8_UNIT_OVERLAY_ID}')")
endif()
if(NOT DQ8_UNIT_PROFILE_ID MATCHES "^[A-Za-z0-9_]+$")
    list(APPEND _problems "DQ8_UNIT_PROFILE_ID must match [A-Za-z0-9_]+ (got '${DQ8_UNIT_PROFILE_ID}')")
endif()
if(NOT DQ8_UNIT_SOURCE_SHA1 MATCHES "^[0-9a-f]+$")
    list(APPEND _problems "DQ8_UNIT_SOURCE_SHA1 must be 40 lowercase hex digits (got '${DQ8_UNIT_SOURCE_SHA1}')")
else()
    string(LENGTH "${DQ8_UNIT_SOURCE_SHA1}" _sha1_length)
    if(NOT _sha1_length EQUAL 40)
        list(APPEND _problems "DQ8_UNIT_SOURCE_SHA1 must be 40 lowercase hex digits (got ${_sha1_length})")
    endif()
endif()
# The generator string lands in a C string literal: no quotes, backslashes or control characters.
if(NOT DQ8_GENERATOR MATCHES "^[A-Za-z0-9 ._:+@/=-]+$")
    list(APPEND _problems "DQ8_GENERATOR must be non-empty and use only [A-Za-z0-9 ._:+@/=-] (got '${DQ8_GENERATOR}')")
endif()
if(NOT DQ8_UNIT_ENTRY_POINT MATCHES "^(0[xX][0-9A-Fa-f]+|[0-9]+)$")
    list(APPEND _problems "DQ8_UNIT_ENTRY_POINT must be 0x... or decimal (got '${DQ8_UNIT_ENTRY_POINT}')")
endif()
if(NOT IS_DIRECTORY "${DQ8_UNIT_GENERATED_DIR}")
    list(APPEND _problems "DQ8_UNIT_GENERATED_DIR is not a directory ('${DQ8_UNIT_GENERATED_DIR}')")
else()
    foreach(_file register_functions.cpp ps2_recompiled_functions.h ps2_recompiled_stubs.h)
        if(NOT EXISTS "${DQ8_UNIT_GENERATED_DIR}/${_file}")
            list(APPEND _problems "${_file} is missing from DQ8_UNIT_GENERATED_DIR ('${DQ8_UNIT_GENERATED_DIR}')")
        endif()
    endforeach()
endif()
foreach(_file dq8/abi/game_module.h dq8/abi/ps2recomp/ps2_runtime.h dq8/abi/ps2recomp/ps2_runtime_macros.h)
    if(NOT EXISTS "${DQ8_ABI_INCLUDE_DIR}/${_file}")
        list(APPEND _problems "DQ8_ABI_INCLUDE_DIR has no ${_file} ('${DQ8_ABI_INCLUDE_DIR}')")
    endif()
endforeach()
set(_zero "^(0[xX]0*|0+)$")
if(DQ8_UNIT_KIND STREQUAL "main")
    set(DQ8_UNIT_KIND_ENUM DQ8_UNIT_MAIN)
    if(NOT DQ8_UNIT_OVERLAY_ID STREQUAL "0")
        list(APPEND _problems "the main unit has DQ8_UNIT_OVERLAY_ID 0 (got '${DQ8_UNIT_OVERLAY_ID}')")
    endif()
    if(DQ8_UNIT_ENTRY_POINT MATCHES "${_zero}")
        list(APPEND _problems "the main unit needs DQ8_UNIT_ENTRY_POINT")
    endif()
else()
    set(DQ8_UNIT_KIND_ENUM DQ8_UNIT_OVERLAY)
    if(NOT DQ8_UNIT_ENTRY_POINT MATCHES "${_zero}")
        list(APPEND _problems "an overlay has DQ8_UNIT_ENTRY_POINT 0 (got '${DQ8_UNIT_ENTRY_POINT}')")
    endif()
endif()

if(_problems)
    list(JOIN _problems "\n  " _text)
    message(FATAL_ERROR "dq8 game module: bad parameters:\n  ${_text}")
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang|MSVC)$")
    message(FATAL_ERROR "dq8 game module: unsupported compiler ${CMAKE_CXX_COMPILER_ID}")
endif()

get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (the module sets its own optimisation flags)" FORCE)
endif()

# ---------------------------------------------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------------------------------------------
set(_target dq8_unit_${DQ8_UNIT_NAME})

# The generated sources: one file per function and per stub, plus register_functions.cpp (the table).
file(GLOB _generated_sources LIST_DIRECTORIES false "${DQ8_UNIT_GENERATED_DIR}/*.cpp")
set(_register_source "${DQ8_UNIT_GENERATED_DIR}/register_functions.cpp")
set(_entry_source "${CMAKE_CURRENT_BINARY_DIR}/dq8_unit_entry.cpp")
configure_file("${CMAKE_CURRENT_LIST_DIR}/dq8_unit_entry.cpp.in" "${_entry_source}" @ONLY)

add_library(${_target} MODULE ${_generated_sources} "${_entry_source}")
set_target_properties(${_target} PROPERTIES
    PREFIX ""
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # A generator expression keeps multi-config generators from adding a per-configuration subdirectory.
    LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/module>")
target_include_directories(${_target} PRIVATE
    "${DQ8_UNIT_GENERATED_DIR}"
    "${DQ8_ABI_INCLUDE_DIR}/dq8/abi/ps2recomp"
    "${DQ8_ABI_INCLUDE_DIR}")

# Every unit defines the same four table globals; give them per-unit names (dq8_unit_entry.cpp.in reads them).
target_compile_definitions(${_target} PRIVATE
    g_ps2RecompiledFunctionTable=dq8_unit_${DQ8_UNIT_NAME}_table
    g_ps2RecompiledFunctionTableBase=dq8_unit_${DQ8_UNIT_NAME}_table_base
    g_ps2RecompiledFunctionTableEnd=dq8_unit_${DQ8_UNIT_NAME}_table_end
    g_ps2RecompiledFunctionTableSlotCount=dq8_unit_${DQ8_UNIT_NAME}_table_slot_count)

# Exceptions cross the module boundary in M2 (the milestone doc's documented deviation), so the module shares the
# host's C++ runtime: no static runtime, and on MSVC /EHs (not /EHsc), since the host's extern "C" callbacks throw.
if(MSVC)
    string(REGEX REPLACE "/EH[a-z]*" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/GR( |$)" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/RTC[1csu]*" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}") # clashes with /O2
    set_target_properties(${_target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    target_compile_options(${_target} PRIVATE /EHs /GR- /O2 /bigobj /utf-8)
    set(_generated_flags /w)
    set(_register_flags /Od)
    if(DQ8_MODULE_DEBUG_INFO)
        target_compile_options(${_target} PRIVATE /Z7)
        target_link_options(${_target} PRIVATE /DEBUG)
    endif()
else()
    target_compile_options(${_target} PRIVATE -O2 -msse4.1 -fexceptions -fno-rtti)
    set(_generated_flags -w)
    set(_register_flags -O0)
    if(DQ8_MODULE_DEBUG_INFO)
        target_compile_options(${_target} PRIVATE -g)
    else()
        target_compile_options(${_target} PRIVATE -g0)
    endif()
    # Windows (llvm-mingw, ADR-0018; cmake/toolchains/llvm-mingw.cmake): a DLL resolves every symbol at link time
    # and exports only what is marked dllexport (DQ8_MODULE_EXPORT), so the ELF linker options below do not apply.
    # libc++ stays a DLL, shared with the host.
    if(NOT APPLE AND NOT WIN32)
        # No undefined references to the host, nothing re-exported from static archives, and one exported
        # symbol: the version script hides everything else, including weak template instantiations that
        # -fvisibility=hidden leaves visible.
        set(_version_script "${CMAKE_CURRENT_BINARY_DIR}/dq8_module.map")
        file(WRITE "${_version_script}" "{\n  global: dq8_game_module_describe;\n  local: *;\n};\n")
        target_link_options(${_target} PRIVATE
            LINKER:-z,defs LINKER:--exclude-libs,ALL "LINKER:--version-script=${_version_script}")
        set_property(TARGET ${_target} APPEND PROPERTY LINK_DEPENDS "${_version_script}")
    endif()
endif()

# Generated code is compiled without warnings (it is not ours to fix); register_functions.cpp is one huge static
# initialiser, which optimisation only slows down.
set_source_files_properties(${_generated_sources} PROPERTIES COMPILE_OPTIONS "${_generated_flags}")
set_source_files_properties("${_register_source}" PROPERTIES
    COMPILE_OPTIONS "${_generated_flags};${_register_flags}"
    SKIP_PRECOMPILE_HEADERS ON)
set_source_files_properties("${_entry_source}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

if(DQ8_MODULE_PCH)
    target_precompile_headers(${_target} PRIVATE
        <stdexcept>
        <ps2_runtime.h>
        <ps2_runtime_macros.h>
        <ps2_syscalls.h>
        <ps2_stubs.h>
        <ps2_recompiled_functions.h>
        <ps2_recompiled_stubs.h>)
endif()

install(TARGETS ${_target} LIBRARY DESTINATION .)
