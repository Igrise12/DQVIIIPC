# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# Builds the pinned recompiler ps2_recomp (third_party/PS2Recomp, ADR-0003) as an external project, offline,
# from a patched copy of the submodule. Included from the top-level CMakeLists.txt when DQ8_BUILD_RECOMP is ON.
#
# Provides:
#   DQ8_PS2_RECOMP_EXECUTABLE  path of the built ps2_recomp (exists once target dq8_ps2recomp has built)
#   dq8::ps2_recomp            imported executable: $<TARGET_FILE:dq8::ps2_recomp>, add_dependencies() work
#   install rules              ps2_recomp next to dq8-game (CMAKE_INSTALL_BINDIR), its licenses in the doc dir
#
# Only ps2xRecomp is configured: PS2Recomp's runtime, analyzer, tests and studio are off, so of its nine
# FetchContent dependencies only the five pinned under third_party/ are used, and
# FETCHCONTENT_FULLY_DISCONNECTED keeps the configure off the network. Upstream builds its tools as Release with
# IPO; our build type, warnings and sanitizers do not apply to it.
include_guard(GLOBAL)
include(ExternalProject)
include(GNUInstallDirs)

set(_dq8_ps2recomp_upstream "${PROJECT_SOURCE_DIR}/third_party/PS2Recomp")
set(_dq8_ps2recomp_patch_dir "${PROJECT_SOURCE_DIR}/patches/ps2recomp")
set(_dq8_ps2recomp_prefix "${CMAKE_BINARY_DIR}/ps2recomp")
set(_dq8_ps2recomp_source "${_dq8_ps2recomp_prefix}/source")
set(_dq8_ps2recomp_binary "${_dq8_ps2recomp_prefix}/build")

if(NOT EXISTS "${_dq8_ps2recomp_upstream}/ps2xRecomp/CMakeLists.txt")
    message(FATAL_ERROR "DQ8_BUILD_RECOMP needs the PS2Recomp submodule: git submodule update --init --recursive")
endif()

set(_dq8_ps2recomp_cache_args
    -DCMAKE_BUILD_TYPE:STRING=Release
    -DFETCHCONTENT_FULLY_DISCONNECTED:BOOL=ON
    -DPS2X_BUILD_RECOMP:BOOL=ON
    -DPS2X_BUILD_RUNTIME:BOOL=OFF
    -DPS2X_BUILD_ANALYZER:BOOL=OFF
    -DPS2X_BUILD_TEST:BOOL=OFF
    -DPS2X_BUILD_STUDIO:BOOL=OFF
    -DPS2X_ENABLE_SCCACHE:BOOL=OFF
    -DBUILD_TESTING:BOOL=OFF)

# FetchContent name -> submodule. The submodules pin the versions; they must match the GIT_TAGs in
# ps2xRecomp/CMakeLists.txt (THIRD_PARTY_NOTICES.md).
foreach(_dep IN ITEMS ELFIO=ELFIO TOML11=toml11 FMT=fmt LIBDWARF=libdwarf-code RABBITIZER=rabbitizer)
    string(REPLACE "=" ";" _dep "${_dep}")
    list(GET _dep 0 _name)
    list(GET _dep 1 _dir)
    file(GLOB _entries "${PROJECT_SOURCE_DIR}/third_party/${_dir}/*")
    if(NOT _entries)
        message(FATAL_ERROR "DQ8_BUILD_RECOMP needs third_party/${_dir}: git submodule update --init --recursive")
    endif()
    list(APPEND _dq8_ps2recomp_cache_args
         "-DFETCHCONTENT_SOURCE_DIR_${_name}:PATH=${PROJECT_SOURCE_DIR}/third_party/${_dir}")
endforeach()

# Same compiler as the host, and its launcher (ccache). Visual Studio and Xcode pick the compiler themselves;
# ExternalProject forwards the generator, platform and toolset.
if(NOT CMAKE_GENERATOR MATCHES "Visual Studio|Xcode")
    list(APPEND _dq8_ps2recomp_cache_args
         "-DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}"
         "-DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}")
endif()
# Cross builds (M5 session E, cmake/toolchains/llvm-mingw.cmake): the same toolchain file, so ps2_recomp targets
# Windows as the host does.
if(CMAKE_CROSSCOMPILING AND CMAKE_TOOLCHAIN_FILE)
    get_filename_component(_dq8_toolchain_file "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    list(APPEND _dq8_ps2recomp_cache_args "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${_dq8_toolchain_file}")
    if(LLVM_MINGW_ROOT)
        list(APPEND _dq8_ps2recomp_cache_args "-DLLVM_MINGW_ROOT:PATH=${LLVM_MINGW_ROOT}")
    endif()
endif()
foreach(_lang C CXX)
    if(CMAKE_${_lang}_COMPILER_LAUNCHER)
        list(APPEND _dq8_ps2recomp_cache_args
             "-DCMAKE_${_lang}_COMPILER_LAUNCHER:STRING=${CMAKE_${_lang}_COMPILER_LAUNCHER}")
    endif()
endforeach()
# Upstream CI's flag on GCC and Clang (its headers use SSE4.1 intrinsics). MSVC x64 needs none; upstream's
# ReleaseMode.cmake adds /arch:AVX2 there.
if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86)$")
    list(APPEND _dq8_ps2recomp_cache_args
         "-DCMAKE_C_FLAGS:STRING=-msse4.1" "-DCMAKE_CXX_FLAGS:STRING=-msse4.1")
endif()

get_property(_dq8_ps2recomp_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
set(DQ8_PS2_RECOMP_EXECUTABLE "${_dq8_ps2recomp_binary}/ps2xRecomp")
if(_dq8_ps2recomp_multi_config)
    string(APPEND DQ8_PS2_RECOMP_EXECUTABLE "/Release")
endif()
string(APPEND DQ8_PS2_RECOMP_EXECUTABLE "/ps2_recomp${CMAKE_EXECUTABLE_SUFFIX}")

# The patch queue. Adding or removing a patch re-runs the configure (CONFIGURE_DEPENDS) and changes the set id
# on the download command, which re-runs that step; an edited patch is newer than the step's stamp.
file(GLOB _dq8_ps2recomp_patches CONFIGURE_DEPENDS LIST_DIRECTORIES false "${_dq8_ps2recomp_patch_dir}/*.patch")
list(SORT _dq8_ps2recomp_patches)
set(_dq8_ps2recomp_patch_names "")
foreach(_patch IN LISTS _dq8_ps2recomp_patches)
    get_filename_component(_name "${_patch}" NAME)
    string(APPEND _dq8_ps2recomp_patch_names "${_name};")
endforeach()
string(SHA1 _dq8_ps2recomp_patch_set_id "${_dq8_ps2recomp_patch_names}")

find_package(Git QUIET)
if(_dq8_ps2recomp_patches AND NOT GIT_EXECUTABLE)
    message(FATAL_ERROR "patches/ps2recomp/ is not empty and git was not found (the queue is applied with git)")
endif()

ExternalProject_Add(dq8_ps2recomp
    PREFIX "${_dq8_ps2recomp_prefix}"
    SOURCE_DIR "${_dq8_ps2recomp_source}"
    BINARY_DIR "${_dq8_ps2recomp_binary}"
    STAMP_DIR "${_dq8_ps2recomp_prefix}/stamp"
    TMP_DIR "${_dq8_ps2recomp_prefix}/tmp"
    LOG_DIR "${_dq8_ps2recomp_prefix}/log"
    DOWNLOAD_COMMAND
        "${CMAKE_COMMAND}"
        "-DUPSTREAM_DIR=${_dq8_ps2recomp_upstream}"
        "-DSOURCE_DIR=${_dq8_ps2recomp_source}"
        "-DBINARY_DIR=${_dq8_ps2recomp_binary}"
        "-DPATCH_DIR=${_dq8_ps2recomp_patch_dir}"
        "-DPATCH_SET_ID=${_dq8_ps2recomp_patch_set_id}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        -P "${CMAKE_CURRENT_LIST_DIR}/Ps2RecompPrepare.cmake"
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
    CMAKE_CACHE_ARGS ${_dq8_ps2recomp_cache_args}
    BUILD_COMMAND "${CMAKE_COMMAND}" --build "${_dq8_ps2recomp_binary}" --config Release --target ps2_recomp
    BUILD_BYPRODUCTS "${DQ8_PS2_RECOMP_EXECUTABLE}"
    INSTALL_COMMAND ""
    LOG_DOWNLOAD ON
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_OUTPUT_ON_FAILURE ON)

# Re-copy the tree when a patch or the prepare script changes, and when the submodule moves to another commit
# (git submodule update rewrites HEAD in the submodule's git dir).
set(_dq8_ps2recomp_download_deps ${_dq8_ps2recomp_patches} "${CMAKE_CURRENT_LIST_DIR}/Ps2RecompPrepare.cmake")
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${_dq8_ps2recomp_upstream}" rev-parse --absolute-git-dir
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _git_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_rc EQUAL 0 AND EXISTS "${_git_dir}/HEAD")
        list(APPEND _dq8_ps2recomp_download_deps "${_git_dir}/HEAD")
    endif()
endif()
ExternalProject_Add_StepDependencies(dq8_ps2recomp download ${_dq8_ps2recomp_download_deps})

add_executable(dq8::ps2_recomp IMPORTED GLOBAL)
set_target_properties(dq8::ps2_recomp PROPERTIES IMPORTED_LOCATION "${DQ8_PS2_RECOMP_EXECUTABLE}")
add_dependencies(dq8::ps2_recomp dq8_ps2recomp)

install(PROGRAMS "${DQ8_PS2_RECOMP_EXECUTABLE}" DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT recompiler)
# ps2_recomp is GPL-3.0 and links MIT and LGPL-2.1 code; their notices travel with the binary.
foreach(_license IN ITEMS
        PS2Recomp/LICENSE=PS2Recomp-LICENSE
        ELFIO/LICENSE.txt=ELFIO-LICENSE.txt
        fmt/LICENSE=fmt-LICENSE
        libdwarf-code/src/lib/libdwarf/LIBDWARFCOPYRIGHT=libdwarf-LIBDWARFCOPYRIGHT
        libdwarf-code/src/lib/libdwarf/LGPL.txt=libdwarf-LGPL.txt
        rabbitizer/LICENSE=rabbitizer-LICENSE
        toml11/LICENSE=toml11-LICENSE)
    string(REPLACE "=" ";" _license "${_license}")
    list(GET _license 0 _from)
    list(GET _license 1 _to)
    install(FILES "${PROJECT_SOURCE_DIR}/third_party/${_from}" RENAME "${_to}"
            DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses/ps2_recomp" COMPONENT recompiler)
endforeach()

message(STATUS "ps2_recomp: built offline from third_party/PS2Recomp by target dq8_ps2recomp "
               "(logs in ${_dq8_ps2recomp_prefix}/log)")
