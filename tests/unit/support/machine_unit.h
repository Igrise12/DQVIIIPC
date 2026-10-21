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
// a fileio read from the synthetic disc through the SIF command and RPC protocol, with a DMAC channel 5
// handler, as the game's libraries do it. dq8-game --elf runs it as a module (dq8_unit_machine) with the
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
inline constexpr std::uint32_t var_vblanks = 0x00280010u;     // incremented by the vblank handler
inline constexpr std::uint32_t var_frames = 0x00280014u;      // incremented by draw_frame
inline constexpr std::uint32_t var_work = 0x00280018u;        // incremented by the worker thread
inline constexpr std::uint32_t var_file_word = 0x0028001Cu;   // the first word read from the disc file
inline constexpr std::uint32_t var_file_status = 0x00280020u; // bytes read, or a negative step code below
inline constexpr std::uint32_t var_completions = 0x00280024u; // fileio completions (0x80000011) handled
inline constexpr std::uint32_t var_rpc_id = 0x00280028u;      // the last RPC request number
inline constexpr std::uint32_t var_iop_buffer = 0x0028002Cu;  // the IOP command buffer
inline constexpr std::uint32_t var_sif0_irqs = 0x00280030u;   // SIF0 handler entries
inline constexpr std::uint32_t sreg_mirror = 0x00280040u;     // 32 words: the EE library's software registers
// Parameter blocks (EE kernel layouts: SemaParam, ThreadParam) and the GIF packet.
inline constexpr std::uint32_t sema_param = 0x00280100u;
inline constexpr std::uint32_t thread_param = 0x00280200u;
inline constexpr std::uint32_t gif_packet = 0x00281000u; // 16-byte aligned
inline constexpr std::uint32_t gif_packet_qwords = 10;
inline constexpr std::uint32_t gif_rgbaq = gif_packet + 7 * 16; // the sprite colour draw_frame updates
// The SIF client's memory (Docs/milestones/M3-boots.md, "Protocol notes"), all 16-byte aligned.
inline constexpr std::uint32_t sif_recv_slot = 0x00282000u;  // 0x80: where IOP packets land
inline constexpr std::uint32_t sif_scratch = 0x00282080u;    // 0x80: the packet being dispatched
inline constexpr std::uint32_t sif_cmd_data = 0x00282100u;   // software register 1's block
inline constexpr std::uint32_t sif_cmd_packet = 0x00282180u; // outgoing system command packets
inline constexpr std::uint32_t sif_desc = 0x00282200u;       // two 16-byte transfer descriptors
inline constexpr std::uint32_t sif_rpc_packet = 0x00282240u; // the one 0x40-byte RPC packet slot
inline constexpr std::uint32_t sif_client = 0x00282280u;     // the 0x28-byte fileio client
inline constexpr std::uint32_t fio_setup_args = 0x002822C0u; // the two record addresses
inline constexpr std::uint32_t fio_reply = 0x002822D0u;      // RPC reply words
inline constexpr std::uint32_t fio_result = 0x002822E0u;     // where a completion's result goes
inline constexpr std::uint32_t fio_records = 0x00282400u;    // two 0x440-byte completion records
inline constexpr std::uint32_t fio_request = 0x00283000u;    // 0x418: the request block
inline constexpr std::uint32_t fio_buffer = 0x00283800u;     // the read's destination
inline constexpr std::uint32_t fio_read_size = 0x100;
// var_file_status when a step fails.
inline constexpr std::int32_t file_status_bind = -101;  // fileio never bound
inline constexpr std::int32_t file_status_setup = -102; // the setup reply's version word is wrong
inline constexpr std::int32_t file_status_call = -103;  // a request was not accepted

inline constexpr std::uint32_t main_stack = 0x01F00000u;
inline constexpr std::uint32_t main_stack_size = 0x00010000u;
inline constexpr std::uint32_t worker_stack = 0x00290000u;
inline constexpr std::uint32_t worker_stack_size = 0x00004000u;
inline constexpr std::uint32_t worker_priority = 60;

// The file the program reads from the synthetic disc (as the ISO builder takes it, and as fileio opens it),
// and its content.
inline constexpr char disc_file[] = "/DATA/SAMPLE.BIN";
inline constexpr char disc_file_path[] = "cdrom0:\\DATA\\SAMPLE.BIN;1";
std::vector<std::uint8_t> disc_file_bytes();

// The data segment's initial bytes (parameter blocks and the GIF packet).
std::vector<std::uint8_t> data_segment();

} // namespace machine_unit

const dq8_game_module_desc* machine_unit_descriptor();
