# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# host.dq8_game_audio_wiring: the audio wiring of dq8-game (src/host/audio_wiring.cpp) on the synthetic machine
# (support/machine_unit.h), whose program never touches the SPU2. With the SPU2 attached but untouched, the
# GS and RAM hashes are the M3 golden values, no `audio` event is written, the end event's audio_hash is the
# SHA-1 of no frames, --audio-dump writes an empty 48 kHz stereo WAV file, --dump-spu2-at writes the 2 MiB
# power-on sound RAM, and --speed 1 gives the same files and events as the default (as fast as possible).
# With SDL_SOUND=ON, a headless run also plays through the SDL sink on SDL's dummy driver.
#   cmake -DDQ8_GAME=<exe> -DELF=<elf> -DMODULE_DIR=<dir> -DISO=<iso> -DWORK_DIR=<dir> -DGS_HASH=<sha1>
#         -DRAM_SHA1=<sha1> [-DSDL_SOUND=ON] -P test_audio_wiring.cmake
cmake_minimum_required(VERSION 3.25)

# The power-on sound RAM: zero but for two blocks of 0x07 bytes at words 0x2800 and 0xE870 (PCSX2's reset).
set(reset_ram_sha1 "a40905c31330eadfe65f86760d8682a3aba9ae39")
set(no_frames_sha1 "da39a3ee5e6b4b0d3255bfef95601890afd80709")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

function(run_game name)
    set(dir "${WORK_DIR}/${name}")
    file(MAKE_DIRECTORY "${dir}")
    execute_process(
        COMMAND "${DQ8_GAME}" --elf "${ELF}" --module-dir "${MODULE_DIR}" --disc "${ISO}" --headless --gs null
                --vblanks 10 --audio-dump "${dir}/audio.wav" --dump-spu2-at "5:${dir}/spu2.bin"
                --events "${dir}/events.jsonl" ${ARGN}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${name}: dq8-game exited ${rc}\n--- stdout\n${out}\n--- stderr\n${err}")
    endif()
    file(READ "${dir}/events.jsonl" events)
    string(REGEX MATCH "{\"ev\":\"end\"[^\n]*" end "${events}")
    string(JSON gs_hash GET "${end}" gs_hash)
    string(JSON ram_sha1 GET "${end}" ram_sha1)
    string(JSON audio_hash GET "${end}" audio_hash)
    if(NOT gs_hash STREQUAL GS_HASH OR NOT ram_sha1 STREQUAL RAM_SHA1)
        message(FATAL_ERROR "${name}: the hashes moved with the SPU2 attached: gs ${gs_hash}, ram ${ram_sha1}")
    endif()
    if(NOT audio_hash STREQUAL no_frames_sha1)
        message(FATAL_ERROR "${name}: audio_hash ${audio_hash}, expected the hash of no frames")
    endif()
    if(events MATCHES "\"ev\":\"audio\"")
        message(FATAL_ERROR "${name}: an audio event although the SPU2 was never used")
    endif()
    string(REGEX MATCH "{\"ev\":\"spu2_dump\"[^\n]*" dump_event "${events}")
    string(JSON dump_sha1 GET "${dump_event}" sha1)
    string(JSON dump_vblank GET "${dump_event}" vblank)
    file(SHA1 "${dir}/spu2.bin" spu2_file_sha1)
    file(SIZE "${dir}/spu2.bin" spu2_size)
    if(NOT dump_sha1 STREQUAL reset_ram_sha1 OR NOT spu2_file_sha1 STREQUAL reset_ram_sha1
       OR NOT spu2_size EQUAL 2097152 OR NOT dump_vblank EQUAL 5)
        message(FATAL_ERROR "${name}: the SPU2 dump (vblank ${dump_vblank}, ${spu2_size} bytes, ${spu2_file_sha1}, "
                            "event ${dump_sha1}) is not the power-on sound RAM after 5 vblanks")
    endif()
    # A 44-byte header: RIFF, size 36, WAVE, PCM, 2 channels, 48000 Hz, 192000 bytes/s, 4-byte frames, 16 bits,
    # data of 0 bytes.
    file(READ "${dir}/audio.wav" wav HEX)
    set(expected_wav "524946462400000057415645666d74201000000001000200")
    string(APPEND expected_wav "80bb000000ee02000400100064617461" "00000000")
    if(NOT wav STREQUAL expected_wav)
        message(FATAL_ERROR "${name}: audio.wav is\n  ${wav}\nexpected\n  ${expected_wav}")
    endif()
    set(${name}_events "${events}" PARENT_SCOPE)
    set(${name}_err "${err}" PARENT_SCOPE)
endfunction()

run_game(max)
run_game(speed1 --speed 1)
if(NOT max_events STREQUAL speed1_events)
    message(FATAL_ERROR "--speed 1 and the default speed wrote different events")
endif()
message(STATUS "ok: the SPU2 attached and untouched leaves the hashes; WAV, dump and events equal at both speeds")

if(SDL_SOUND)
    set(ENV{SDL_AUDIO_DRIVER} "dummy")
    run_game(sdl --audio sdl --audio-latency 20)
    if(NOT sdl_err MATCHES "audio: [0-9]+ frames played, [0-9]+ underruns")
        message(FATAL_ERROR "--audio sdl: no sink report on stderr:\n${sdl_err}")
    endif()
    if(NOT sdl_events STREQUAL max_events)
        message(FATAL_ERROR "--audio sdl wrote other events than the null sink")
    endif()
    message(STATUS "ok: --audio sdl on the dummy driver gives the null sink's events")
endif()
