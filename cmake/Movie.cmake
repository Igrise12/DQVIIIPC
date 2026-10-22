# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# The movie decoder library dq8-movie (ADR-0015, Docs/milestones/M4-playable.md session C) and where its FFmpeg
# comes from. Included once, from src/runtime/movie/movie.cmake.
#
#   DQ8_FFMPEG=auto    build dq8-movie when pkg-config finds libavcodec and libavutil, else skip it (default)
#   DQ8_FFMPEG=system  require them (configuration fails without them)
#   DQ8_FFMPEG=off     never build dq8-movie
#
# MSVC builds have no dq8-movie yet (a bundled LGPL FFmpeg and Windows DLLs come with packaging, M5). Without the
# library, dq8-game still builds and runs: --movies auto skips the movies and --movies play exits 3.
#
# Defines DQ8_BUILD_MOVIE (ON when the library is built; in src/runtime's scope only, elsewhere test
# `if(TARGET dq8_movie)`) and, then, the imported target PkgConfig::DQ8_LIBAV.
set(DQ8_FFMPEG "auto" CACHE STRING "FFmpeg for the dq8-movie library: auto, system or off")
set_property(CACHE DQ8_FFMPEG PROPERTY STRINGS auto system off)
if(NOT DQ8_FFMPEG MATCHES "^(auto|system|off)$")
    message(FATAL_ERROR "DQ8_FFMPEG must be auto, system or off, not '${DQ8_FFMPEG}'")
endif()

set(DQ8_BUILD_MOVIE OFF)
if(DQ8_FFMPEG STREQUAL "off")
    message(STATUS "dq8-movie: off (DQ8_FFMPEG=off)")
elseif(MSVC)
    if(DQ8_FFMPEG STREQUAL "system")
        message(FATAL_ERROR "DQ8_FFMPEG=system: dq8-movie is not built with MSVC before packaging (M5)")
    endif()
    message(STATUS "dq8-movie: off (MSVC)")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(DQ8_LIBAV QUIET IMPORTED_TARGET libavcodec libavutil)
    endif()
    if(DQ8_LIBAV_FOUND)
        set(DQ8_BUILD_MOVIE ON)
        message(STATUS "dq8-movie: on (libavcodec ${DQ8_LIBAV_libavcodec_VERSION}, "
                       "libavutil ${DQ8_LIBAV_libavutil_VERSION})")
    elseif(DQ8_FFMPEG STREQUAL "system")
        message(FATAL_ERROR "DQ8_FFMPEG=system: pkg-config found no libavcodec and libavutil "
                            "(Debian/Ubuntu: libavcodec-dev libavutil-dev)")
    else()
        message(STATUS "dq8-movie: off (pkg-config found no libavcodec and libavutil)")
    endif()
endif()
