# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# dq8-movie: our C ABI wrapper over the system's libavcodec (mpeg2video) and libavutil, one shared library that
# exports only dq8_mvd_* (ADR-0015). dq8-game loads it at run time (runtime/movie/movie_library.cpp), so the host
# never links FFmpeg and a user can rebuild and replace this one file (NOTICE.md). Target dq8_movie, file
# dq8-movie with the platform's prefix and suffix (libdq8-movie.so), built next to dq8-game. Configured by
# cmake/Movie.cmake (DQ8_FFMPEG).
add_library(dq8_movie SHARED dq8_movie.cpp)
target_include_directories(dq8_movie PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(dq8_movie PRIVATE cxx_std_20)
target_link_libraries(dq8_movie PRIVATE PkgConfig::DQ8_LIBAV)
set_target_properties(dq8_movie PROPERTIES
    OUTPUT_NAME dq8-movie
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:dq8-game>"
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:dq8-game>")
dq8_set_warnings(dq8_movie)
dq8_enable_sanitizers(dq8_movie)
if(WIN32)
    # dq8-movie.dll, the name movie_library.cpp looks for; MinGW would add a "lib" prefix (M5 session E).
    set_target_properties(dq8_movie PROPERTIES PREFIX "")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME MATCHES "BSD")
    set(_dq8_mvd_map "${CMAKE_CURRENT_SOURCE_DIR}/dq8_mvd.map")
    target_link_options(dq8_movie PRIVATE "LINKER:--version-script=${_dq8_mvd_map}")
    set_property(TARGET dq8_movie APPEND PROPERTY LINK_DEPENDS "${_dq8_mvd_map}")
    # Every symbol must resolve inside the library or FFmpeg. Not with sanitizers: their runtime is only
    # resolved when an instrumented executable loads the library.
    if(NOT DQ8_SANITIZERS)
        target_link_options(dq8_movie PRIVATE "LINKER:--no-undefined")
    endif()
endif()

# Our notice travels with the library, in the build tree and in an install.
add_custom_command(TARGET dq8_movie POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.md"
        "$<TARGET_FILE_DIR:dq8_movie>/dq8-movie.NOTICE.md"
    VERBATIM)

include(GNUInstallDirs)
install(TARGETS dq8_movie
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT game
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT game)
install(FILES NOTICE.md DESTINATION "${CMAKE_INSTALL_DOCDIR}/dq8-movie" COMPONENT game)
