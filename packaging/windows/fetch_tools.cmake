# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#
# Downloads and unpacks the pinned Windows build tools (cmake/toolchains/llvm-mingw-pin.cmake) for the cross build
# and the zip: the Linux-hosted llvm-mingw, and the Windows cmake and ninja that ship in the zip. Every file is
# checked against its pinned SHA-256; an archive already present with the right digest is not fetched again.
#
#   cmake -DDEST=<dir> [-DTOOLS=llvm-mingw;cmake;ninja] -P packaging/windows/fetch_tools.cmake
#
# Results (also printed): <DEST>/llvm-mingw-<version>-ucrt-ubuntu-22.04-x86_64 (LLVM_MINGW_ROOT),
# <DEST>/cmake-<version>-windows-x86_64 and <DEST>/ninja-<version>/ (ninja.exe, COPYING). Contributor and CI tool
# only.
cmake_minimum_required(VERSION 3.25)
if(NOT DEST)
    message(FATAL_ERROR "usage: cmake -DDEST=<dir> [-DTOOLS=llvm-mingw;cmake;ninja] -P fetch_tools.cmake")
endif()
if(NOT TOOLS)
    set(TOOLS llvm-mingw cmake ninja)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/toolchains/llvm-mingw-pin.cmake")
file(TO_CMAKE_PATH "${DEST}" DEST)
get_filename_component(DEST "${DEST}" ABSOLUTE)
file(MAKE_DIRECTORY "${DEST}/downloads")

# fetch(<url> <file name> <sha256>): <DEST>/downloads/<file name>, verified.
function(fetch url name sha256)
    set(file "${DEST}/downloads/${name}")
    if(EXISTS "${file}")
        file(SHA256 "${file}" have)
        if(have STREQUAL sha256)
            return()
        endif()
        file(REMOVE "${file}")
    endif()
    message(STATUS "Downloading ${url}")
    file(DOWNLOAD "${url}" "${file}.part" EXPECTED_HASH SHA256=${sha256} STATUS status TLS_VERIFY ON)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        file(REMOVE "${file}.part")
        message(FATAL_ERROR "download of ${url} failed: ${status}")
    endif()
    file(RENAME "${file}.part" "${file}")
endfunction()

# unpack(<file name> <directory the archive creates>): into <DEST>, once.
function(unpack name dir)
    if(IS_DIRECTORY "${DEST}/${dir}")
        return()
    endif()
    file(ARCHIVE_EXTRACT INPUT "${DEST}/downloads/${name}" DESTINATION "${DEST}/${dir}.unpack")
    file(RENAME "${DEST}/${dir}.unpack/${dir}" "${DEST}/${dir}")
    file(REMOVE_RECURSE "${DEST}/${dir}.unpack")
endfunction()

if("llvm-mingw" IN_LIST TOOLS)
    fetch("${DQ8_LLVM_MINGW_RELEASE}/${DQ8_LLVM_MINGW_LINUX_ARCHIVE}" "${DQ8_LLVM_MINGW_LINUX_ARCHIVE}"
          "${DQ8_LLVM_MINGW_LINUX_SHA256}")
    string(REGEX REPLACE "\\.tar\\.xz$" "" _dir "${DQ8_LLVM_MINGW_LINUX_ARCHIVE}")
    unpack("${DQ8_LLVM_MINGW_LINUX_ARCHIVE}" "${_dir}")
    message(STATUS "LLVM_MINGW_ROOT=${DEST}/${_dir}")
endif()
if("cmake" IN_LIST TOOLS)
    fetch("${DQ8_WINDOWS_CMAKE_URL}" "${DQ8_WINDOWS_CMAKE_ARCHIVE}" "${DQ8_WINDOWS_CMAKE_SHA256}")
    string(REGEX REPLACE "\\.zip$" "" _dir "${DQ8_WINDOWS_CMAKE_ARCHIVE}")
    unpack("${DQ8_WINDOWS_CMAKE_ARCHIVE}" "${_dir}")
    message(STATUS "DQ8_WINDOWS_CMAKE=${DEST}/${_dir}/bin/cmake.exe")
endif()
if("ninja" IN_LIST TOOLS)
    set(_ninja_archive "ninja-${DQ8_WINDOWS_NINJA_VERSION}-win.zip")
    fetch("${DQ8_WINDOWS_NINJA_URL}" "${_ninja_archive}" "${DQ8_WINDOWS_NINJA_SHA256}")
    set(_dir "ninja-${DQ8_WINDOWS_NINJA_VERSION}")
    if(NOT EXISTS "${DEST}/${_dir}/ninja.exe")
        file(ARCHIVE_EXTRACT INPUT "${DEST}/downloads/${_ninja_archive}" DESTINATION "${DEST}/${_dir}")
    endif()
    fetch("${DQ8_WINDOWS_NINJA_COPYING_URL}" "ninja-${DQ8_WINDOWS_NINJA_VERSION}-COPYING"
          "${DQ8_WINDOWS_NINJA_COPYING_SHA256}")
    file(COPY_FILE "${DEST}/downloads/ninja-${DQ8_WINDOWS_NINJA_VERSION}-COPYING" "${DEST}/${_dir}/COPYING")
    message(STATUS "DQ8_WINDOWS_NINJA=${DEST}/${_dir}/ninja.exe")
endif()
