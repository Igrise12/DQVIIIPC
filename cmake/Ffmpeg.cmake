# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# A minimal LGPL build of FFmpeg for dq8-movie (ADR-0015; DQ8_FFMPEG=bundled in cmake/Movie.cmake): libavcodec
# with the mpeg2video decoder only, and libavutil, as shared libraries, from the pinned submodule third_party/ffmpeg
# (a release tag; `update = none` in .gitmodules, so fetch it with
# `git submodule update --init --checkout --depth 1 third_party/ffmpeg`). Built with FFmpeg's own configure and
# make, so a POSIX shell and make are needed; nasm is used when found (FFmpeg's x86 assembly), else the C code.
#
# No GPL or non-free parts and no autodetected system libraries, so the result is LGPL-2.1-or-later. The libraries
# are installed into lib/ next to dq8-movie, with FFmpeg's licence text and the configure line in
# share/doc/<project>/ffmpeg/. Included once, from cmake/Movie.cmake.
#
# Defines the interface target dq8_ffmpeg_bundled (include directory, both libraries, the build as a dependency).
set(_dq8_ffmpeg_source "${PROJECT_SOURCE_DIR}/third_party/ffmpeg")
if(NOT EXISTS "${_dq8_ffmpeg_source}/configure")
    message(FATAL_ERROR "DQ8_FFMPEG=bundled needs the FFmpeg submodule: "
                        "git submodule update --init --checkout --depth 1 third_party/ffmpeg")
endif()
if(CMAKE_HOST_WIN32)
    message(FATAL_ERROR "DQ8_FFMPEG=bundled builds FFmpeg with its configure script: use a Linux host "
                        "(cross-building for Windows works from Linux)")
endif()
find_program(DQ8_MAKE_PROGRAM NAMES gmake make REQUIRED)

include(ExternalProject)
set(_dq8_ffmpeg_prefix "${CMAKE_BINARY_DIR}/ffmpeg")
set(_dq8_ffmpeg_install "${_dq8_ffmpeg_prefix}/install")
# FFmpeg's configure and Makefiles cannot work in paths with whitespace; such a tree is reached through a
# space-free symbolic link in the temporary directory, re-created by every step.
set(_dq8_ffmpeg_link "${_dq8_ffmpeg_prefix}")
if(_dq8_ffmpeg_prefix MATCHES "[ \t]" OR _dq8_ffmpeg_source MATCHES "[ \t]")
    string(SHA1 _dq8_ffmpeg_hash "${_dq8_ffmpeg_prefix}")
    string(SUBSTRING "${_dq8_ffmpeg_hash}" 0 12 _dq8_ffmpeg_hash)
    set(_dq8_ffmpeg_tmp "/tmp")
    if(DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" MATCHES "[ \t]")
        set(_dq8_ffmpeg_tmp "$ENV{TMPDIR}")
    endif()
    set(_dq8_ffmpeg_link "${_dq8_ffmpeg_tmp}/dq8-ffmpeg-${_dq8_ffmpeg_hash}")
endif()
if(_dq8_ffmpeg_prefix MATCHES "'" OR _dq8_ffmpeg_source MATCHES "'")
    message(FATAL_ERROR "DQ8_FFMPEG=bundled: the build and source paths must not contain a single quote")
endif()
set(_dq8_ffmpeg_args
    "--prefix=${_dq8_ffmpeg_link}/install"
    --enable-shared --disable-static --enable-pic
    --disable-programs --disable-doc --disable-debug
    --disable-autodetect --disable-network
    --disable-everything --enable-decoder=mpeg2video
    --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample
    "--cc=${CMAKE_C_COMPILER}")
find_program(DQ8_NASM nasm)
if(NOT DQ8_NASM)
    list(APPEND _dq8_ffmpeg_args --disable-x86asm)
endif()
if(CMAKE_CROSSCOMPILING)
    # FFmpeg's names for the target (M5 session E: llvm-mingw).
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _dq8_ffmpeg_arch)
    set(_dq8_ffmpeg_os "${CMAKE_SYSTEM_NAME}")
    if(WIN32)
        set(_dq8_ffmpeg_os mingw32)
    endif()
    string(TOLOWER "${_dq8_ffmpeg_os}" _dq8_ffmpeg_os)
    list(APPEND _dq8_ffmpeg_args --enable-cross-compile "--arch=${_dq8_ffmpeg_arch}" "--target-os=${_dq8_ffmpeg_os}")
    if(CMAKE_AR)
        list(APPEND _dq8_ffmpeg_args "--ar=${CMAKE_AR}")
    endif()
    if(CMAKE_RANLIB)
        list(APPEND _dq8_ffmpeg_args "--ranlib=${CMAKE_RANLIB}")
    endif()
endif()
cmake_host_system_information(RESULT _dq8_ffmpeg_jobs QUERY NUMBER_OF_LOGICAL_CORES)

# One script runs each step from <link>/build with the source at <link>/source.
set(_dq8_ffmpeg_quoted "")
foreach(_arg IN LISTS _dq8_ffmpeg_args)
    string(APPEND _dq8_ffmpeg_quoted " '${_arg}'")
endforeach()
file(WRITE "${_dq8_ffmpeg_prefix}/step.sh"
    "#!/bin/sh\n# Written by cmake/Ffmpeg.cmake: one step of the bundled FFmpeg build (configure, build, install).\n"
    "set -e\nreal='${_dq8_ffmpeg_prefix}'\nlink='${_dq8_ffmpeg_link}'\n"
    "mkdir -p \"$real/build\" \"$real/install\"\n"
    "ln -sfn '${_dq8_ffmpeg_source}' \"$real/source\"\n"
    "if [ \"$link\" != \"$real\" ]; then ln -sfn \"$real\" \"$link\"; fi\n"
    "cd \"$link/build\"\n"
    "case \"$1\" in\n"
    "configure) exec \"$link/source/configure\"${_dq8_ffmpeg_quoted} ;;\n"
    "build) exec '${DQ8_MAKE_PROGRAM}' -j${_dq8_ffmpeg_jobs} ;;\n"
    "install) exec '${DQ8_MAKE_PROGRAM}' install ;;\n"
    "esac\nexit 2\n")

ExternalProject_Add(dq8_ffmpeg_build
    SOURCE_DIR "${_dq8_ffmpeg_source}"
    PREFIX "${_dq8_ffmpeg_prefix}"
    BINARY_DIR "${_dq8_ffmpeg_prefix}/build"
    INSTALL_DIR "${_dq8_ffmpeg_install}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND sh "${_dq8_ffmpeg_prefix}/step.sh" configure
    BUILD_COMMAND sh "${_dq8_ffmpeg_prefix}/step.sh" build
    INSTALL_COMMAND sh "${_dq8_ffmpeg_prefix}/step.sh" install
    LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON LOG_OUTPUT_ON_FAILURE ON
    USES_TERMINAL_BUILD OFF)

if(WIN32)
    # FFmpeg's shared build on mingw: import libraries in lib/, the DLLs in bin/.
    set(_dq8_avcodec "${_dq8_ffmpeg_install}/lib/libavcodec.dll.a")
    set(_dq8_avutil "${_dq8_ffmpeg_install}/lib/libavutil.dll.a")
else()
    set(_dq8_avcodec "${_dq8_ffmpeg_install}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}avcodec${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(_dq8_avutil "${_dq8_ffmpeg_install}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}avutil${CMAKE_SHARED_LIBRARY_SUFFIX}")
endif()
ExternalProject_Add_Step(dq8_ffmpeg_build byproducts
    DEPENDEES install
    COMMAND "${CMAKE_COMMAND}" -E true
    BYPRODUCTS "${_dq8_avcodec}" "${_dq8_avutil}")
file(MAKE_DIRECTORY "${_dq8_ffmpeg_install}/include")

add_library(dq8_ffmpeg_bundled INTERFACE)
add_dependencies(dq8_ffmpeg_bundled dq8_ffmpeg_build)
target_include_directories(dq8_ffmpeg_bundled SYSTEM INTERFACE "${_dq8_ffmpeg_install}/include")
target_link_libraries(dq8_ffmpeg_bundled INTERFACE "${_dq8_avcodec}" "${_dq8_avutil}")

include(GNUInstallDirs)
if(WIN32)
    install(DIRECTORY "${_dq8_ffmpeg_install}/bin/" DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT game
            FILES_MATCHING PATTERN "avcodec-*.dll" PATTERN "avutil-*.dll")
else()
    install(DIRECTORY "${_dq8_ffmpeg_install}/lib/" DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT game
            FILES_MATCHING PATTERN "libavcodec.so*" PATTERN "libavutil.so*" PATTERN "libavcodec*.dylib"
                           PATTERN "libavutil*.dylib" PATTERN "pkgconfig" EXCLUDE)
endif()
string(REPLACE ";" " " _dq8_ffmpeg_line "configure ${_dq8_ffmpeg_args}")
string(REPLACE "${_dq8_ffmpeg_link}/install" "<prefix>" _dq8_ffmpeg_line "${_dq8_ffmpeg_line}")
string(REPLACE "${CMAKE_C_COMPILER}" "<cc>" _dq8_ffmpeg_line "${_dq8_ffmpeg_line}")
file(WRITE "${_dq8_ffmpeg_prefix}/BUILD.txt"
    "FFmpeg for dq8-movie (DQ8-Native, cmake/Ffmpeg.cmake)\n"
    "Source: https://github.com/FFmpeg/FFmpeg, the release tag pinned as third_party/ffmpeg in DQ8-Native's\n"
    "repository at the release's commit (also https://ffmpeg.org/releases/).\n"
    "Licence: LGPL-2.1-or-later (COPYING.LGPLv2.1 here).\n"
    "Built with:\n  ${_dq8_ffmpeg_line}\n"
    "To use another build, see dq8-movie's NOTICE.md: the libraries are ordinary shared libraries and may be\n"
    "replaced by any compatible build.\n")
install(FILES "${_dq8_ffmpeg_source}/COPYING.LGPLv2.1" "${_dq8_ffmpeg_prefix}/BUILD.txt"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}/ffmpeg" COMPONENT game)
message(STATUS "dq8-movie: bundled FFmpeg from third_party/ffmpeg (mpeg2video only, LGPL; build in ${_dq8_ffmpeg_prefix})")
