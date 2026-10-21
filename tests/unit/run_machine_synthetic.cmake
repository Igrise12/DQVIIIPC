# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# host.dq8_game_machine_synthetic: runs the machine unit (support/machine_unit.h) for VBLANKS vblanks, headless,
# with the trace backend. The checks:
#   - the exit status and the golden GS stream and RAM hashes;
#   - in the RAM dump, the program's own counters: the vblank handler ran once per vblank before the last,
#     the worker thread woke once per drawn frame, and the fileio read returned the file's first word;
#   - in the GS trace, one 160-byte PATH3 transfer (the sprite packet) per drawn frame.
#   cmake -DDQ8_GAME=<exe> -DELF=<elf> -DMODULE_DIR=<dir> -DISO=<iso> -DWORK_DIR=<dir> -DVBLANKS=<n>
#         -DGS_HASH=<sha1> -DRAM_SHA1=<sha1> -P run_machine_synthetic.cmake
cmake_minimum_required(VERSION 3.25)

file(MAKE_DIRECTORY "${WORK_DIR}")
set(ram "${WORK_DIR}/machine.ram")
set(trace "${WORK_DIR}/machine.gs")
execute_process(
    COMMAND "${DQ8_GAME}" --elf "${ELF}" --module-dir "${MODULE_DIR}" --disc "${ISO}" --headless --gs trace
            --gs-trace "${trace}" --vblanks "${VBLANKS}" --dump-ram-at "${VBLANKS}:${ram}" --json
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "dq8-game exited ${rc}, expected 0\n--- stdout\n${out}\n--- stderr\n${err}")
endif()
string(STRIP "${out}" out)
string(REGEX MATCH "[^\n]*$" report "${out}")
string(JSON ended GET "${report}" ended)
string(JSON vblanks GET "${report}" machine vblanks)
string(JSON gs_hash GET "${report}" machine gs_hash)
string(JSON ram_sha1 GET "${report}" machine ram_sha1)
if(NOT ended STREQUAL "vblanks" OR NOT vblanks EQUAL VBLANKS)
    message(FATAL_ERROR "the run ended '${ended}' after ${vblanks} vblanks, expected 'vblanks' after ${VBLANKS}")
endif()

# A little-endian word of the RAM dump.
function(ram_word out_var address)
    math(EXPR offset "${address}") # file(READ) wants a decimal offset
    file(READ "${ram}" hex OFFSET ${offset} LIMIT 4 HEX)
    string(SUBSTRING "${hex}" 6 2 b3)
    string(SUBSTRING "${hex}" 4 2 b2)
    string(SUBSTRING "${hex}" 2 2 b1)
    string(SUBSTRING "${hex}" 0 2 b0)
    math(EXPR value "0x${b3}${b2}${b1}${b0}")
    set(${out_var} "${value}" PARENT_SCOPE)
endfunction()
function(expect what actual expected)
    if(NOT actual EQUAL expected)
        message(FATAL_ERROR "${what}: ${actual}, expected ${expected}")
    endif()
    message(STATUS "ok: ${what} = ${actual}")
endfunction()

# Addresses: support/machine_unit.h.
ram_word(handler_runs 0x280010)
ram_word(frames 0x280014)
ram_word(worker_wakes 0x280018)
ram_word(file_word 0x28001C)
ram_word(file_status 0x280020)
ram_word(completions 0x280024)
math(EXPR expected_handler_runs "${VBLANKS} - 1")
expect("vblank handler runs" "${handler_runs}" "${expected_handler_runs}")
if(frames LESS 1)
    message(FATAL_ERROR "no frame was drawn")
endif()
expect("worker thread wakes" "${worker_wakes}" "${frames}")
expect("fileio read result (bytes)" "${file_status}" 256)
expect("fileio completions (open, read, close)" "${completions}" 3)
expect("first word of the disc file" "${file_word}" 0x18110A03)

# The trace backend's stream (src/runtime/gs/gs_trace_format.h): a transfer record is {u8 0, u8 path, u32 size,
# data}, with PCSX2's path numbering (2 = PATH3). The sprite packet is 0xA0 bytes. Count its headers.
file(READ "${trace}" stream HEX)
string(REGEX MATCHALL "0002a0000000" headers "${stream}")
list(LENGTH headers transfers)
expect("PATH3 sprite transfers in the GS trace" "${transfers}" "${frames}")

if(NOT gs_hash STREQUAL GS_HASH OR NOT ram_sha1 STREQUAL RAM_SHA1)
    message(FATAL_ERROR "hashes differ from the golden values:\n  gs_hash  ${gs_hash} (golden ${GS_HASH})\n"
                        "  ram_sha1 ${ram_sha1} (golden ${RAM_SHA1})")
endif()
message(STATUS "ok: ${frames} frames, gs_hash ${gs_hash}, ram_sha1 ${ram_sha1}")
