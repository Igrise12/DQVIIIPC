# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M4 session B: the SPU2 and audio output tests (test_spu2_*, test_audio_*).
target_sources(dq8_unit_tests PRIVATE
    test_audio_output.cpp
    test_spu2_adpcm.cpp
    test_spu2_chip.cpp
    test_spu2_device.cpp
)

# The SDL3 audio sink on SDL's dummy audio driver: its own executable, like the platform tests, since
# dq8_unit_tests does not link SDL3. No sound card is needed.
if(DQ8_BUILD_WINDOW)
    add_executable(dq8_audio_sdl_tests test_audio_sdl.cpp)
    target_link_libraries(dq8_audio_sdl_tests PRIVATE dq8::platform_sdl Catch2::Catch2WithMain)
    dq8_set_warnings(dq8_audio_sdl_tests)
    dq8_enable_sanitizers(dq8_audio_sdl_tests)
    include(Catch)
    catch_discover_tests(dq8_audio_sdl_tests PROPERTIES LABELS unit ENVIRONMENT "SDL_AUDIO_DRIVER=dummy")
endif()

# dq8-game's audio wiring on the synthetic machine (test_audio_wiring.cmake): the SPU2 attached and untouched,
# --audio-dump, --dump-spu2-at, --speed 1 against the default, and --audio sdl on the dummy driver. The machine
# inputs, golden hashes and test fixture come from CMakeLists.txt, which includes this file first: the test is
# added at the end of the directory's processing.
cmake_language(DEFER CALL dq8_add_audio_wiring_test)
function(dq8_add_audio_wiring_test)
    set(sdl_sound OFF)
    if(DQ8_BUILD_WINDOW)
        set(sdl_sound ON)
    endif()
    add_test(NAME host.dq8_game_audio_wiring
        COMMAND "${CMAKE_COMMAND}"
            "-DDQ8_GAME=${DQ8_TEST_GAME}"
            "-DELF=${CMAKE_CURRENT_BINARY_DIR}/machine.elf"
            "-DMODULE_DIR=$<TARGET_FILE_DIR:dq8_unit_machine>"
            "-DISO=${CMAKE_CURRENT_BINARY_DIR}/machine.iso"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/audio_wiring"
            "-DGS_HASH=${DQ8_MACHINE_SYNTHETIC_GS_HASH}"
            "-DRAM_SHA1=${DQ8_MACHINE_SYNTHETIC_RAM_SHA1}"
            "-DSDL_SOUND=${sdl_sound}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/test_audio_wiring.cmake")
    set_tests_properties(host.dq8_game_audio_wiring PROPERTIES FIXTURES_REQUIRED dq8_machine_inputs LABELS unit)
endfunction()
