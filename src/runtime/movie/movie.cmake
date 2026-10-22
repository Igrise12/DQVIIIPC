# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M4 session C: the sceMpeg HLE, the IPU register stub and the dq8-movie loader. Joins dq8_runtime here; the
# decoder library itself (movie/decoder) is built when cmake/Movie.cmake finds FFmpeg.
target_sources(dq8_runtime PRIVATE
    movie/ipu_regs.cpp
    movie/mpeg_es_model.cpp
    movie/ipu_csc.cpp
    movie/macroblock_writer.cpp
    movie/movie_decoder.cpp
    movie/movie_library.cpp
    movie/sce_mpeg_hle.cpp
)
include(Movie)
if(DQ8_BUILD_MOVIE)
    add_subdirectory(movie/decoder)
    # The build tree's copy: the loader's last place to look, so development builds need no DQ8_MOVIE_LIBRARY.
    set_source_files_properties(movie/movie_library.cpp PROPERTIES
        COMPILE_DEFINITIONS "DQ8_MOVIE_BUILD_PATH=\"$<TARGET_FILE:dq8_movie>\"")
endif()
