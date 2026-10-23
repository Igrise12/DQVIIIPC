# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M4 session C: the movie tests (test_movie_*, test_ipu_*, test_sce_mpeg_hle.cpp). Streams come from the
# test-time MPEG-2 writer (support/mpeg2_writer.*); no fixture holds movie data. With dq8-movie built
# (cmake/Movie.cmake), the decoder tests load it by its build path; without it they SKIP.
target_sources(dq8_unit_tests PRIVATE
    support/mpeg2_writer.cpp
    test_movie_es_model.cpp
    test_ipu_csc.cpp
    test_movie_decoder.cpp
    test_sce_mpeg_hle.cpp
    test_movie_policy.cpp
)
if(TARGET dq8_movie)
    set_source_files_properties(test_movie_decoder.cpp PROPERTIES
        COMPILE_DEFINITIONS "DQ8_MOVIE_LIBRARY=\"$<TARGET_FILE:dq8_movie>\"")
    add_dependencies(dq8_unit_tests dq8_movie)
endif()

# dq8-game: --movies play without the library is a start-up failure, exit 3 (BackendError).
add_test(NAME host.dq8_game_movies_play_without_library
    COMMAND "${CMAKE_COMMAND}"
        "-DDQ8_GAME=${DQ8_TEST_GAME}"
        "-DARGS=--elf@@${CMAKE_CURRENT_BINARY_DIR}/contract.elf@@--module-dir@@$<TARGET_FILE_DIR:dq8_unit_contract>@@--movies@@play@@--movie-decoder@@${CMAKE_CURRENT_BINARY_DIR}/no-such-libdq8-movie.so@@--stop-at-first-syscall"
        -DEXPECT_EXIT=3
        "-DEXPECT=--movies play: cannot load the movie decoder library"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_and_match.cmake")
set_tests_properties(host.dq8_game_movies_play_without_library PROPERTIES FIXTURES_REQUIRED dq8_contract_elf
                                                                          LABELS unit)
