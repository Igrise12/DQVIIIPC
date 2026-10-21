// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/abi/game_module.h"

#include <cstdint>
#include <vector>

// The machine unit: a synthetic multi-thread program, written by hand in the shape PS2Recomp's emitter
// produces (see contract_unit.h), that exercises the M3 machine the way a game does: SetupThread, two
// semaphores, a vblank-start INTC handler that iSignalSemas, a worker thread, the GS CRT and privileged
// registers, a GIF DMA (channel 2) of a sprite packet polled to completion at a back-edge checkpoint, and
// a fileio read from the synthetic disc. dq8-game --elf runs it as a module (dq8_unit_machine) with the
// ELF and the ISO that write_machine_inputs.cpp writes; CTest host.dq8_game_machine_synthetic compares the
// GS stream and RAM hashes after 10 vblanks with golden values. Every address, value and instruction is
// ours.
namespace machine_unit {

inline constexpr std::uint32_t table_base = 0x00200000u;
inline constexpr std::uint32_t table_end = 0x00200800u;
inline constexpr char unit_name[] = "machine";
inline constexpr char source_sha1[] = "6d616368696e652d756e69742d73796e74686574";

inline constexpr std::uint32_t entry = 0x00200000u;

// The data segment the ELF carries: [data_base, data_end).
inline constexpr std::uint32_t data_base = 0x00280000u;
inline constexpr std::uint32_t data_end = 0x00284000u;
// Words the program keeps (offsets from data_base): semaphore ids, handler id, thread id, counters.
inline constexpr std::uint32_t var_vsync_sema = 0x00280000u;
inline constexpr std::uint32_t var_work_sema = 0x00280004u;
inline constexpr std::uint32_t var_handler = 0x00280008u;
inline constexpr std::uint32_t var_worker = 0x0028000Cu;
inline constexpr std::uint32_t var_vblanks = 0x00280010u;   // incremented by the vblank handler
inline constexpr std::uint32_t var_frames = 0x00280014u;    // incremented by draw_frame
inline constexpr std::uint32_t var_work = 0x00280018u;      // incremented by the worker thread
inline constexpr std::uint32_t var_file_word = 0x0028001Cu; // the first word read from the disc file
// Parameter blocks (EE kernel layouts: SemaParam, ThreadParam) and the GIF packet.
inline constexpr std::uint32_t sema_param = 0x00280100u;
inline constexpr std::uint32_t thread_param = 0x00280200u;
inline constexpr std::uint32_t gif_packet = 0x00281000u; // 16-byte aligned
inline constexpr std::uint32_t gif_packet_qwords = 9;
inline constexpr std::uint32_t gif_rgbaq = gif_packet + 6 * 16; // the sprite colour draw_frame updates

inline constexpr std::uint32_t main_stack = 0x01F00000u;
inline constexpr std::uint32_t main_stack_size = 0x00010000u;
inline constexpr std::uint32_t worker_stack = 0x00290000u;
inline constexpr std::uint32_t worker_stack_size = 0x00004000u;
inline constexpr std::uint32_t worker_priority = 60;

// The file the program reads from the synthetic disc, and its content.
inline constexpr char disc_file[] = "/DATA/SAMPLE.BIN";
std::vector<std::uint8_t> disc_file_bytes();

// The data segment's initial bytes (parameter blocks and the GIF packet).
std::vector<std::uint8_t> data_segment();

} // namespace machine_unit

const dq8_game_module_desc* machine_unit_descriptor();
