// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The machine unit's data segment and disc file (machine_unit.h): parameter blocks in the EE kernel's
// layouts and a GIF packet, all with our own values.
#include "support/machine_unit.h"

namespace machine_unit {

namespace {

void put32(std::vector<std::uint8_t>& out, std::uint32_t addr, std::uint32_t v) {
    const std::size_t at = addr - data_base;
    for (int i = 0; i < 4; ++i) {
        out[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

void put64(std::vector<std::uint8_t>& out, std::uint32_t addr, std::uint64_t v) {
    put32(out, addr, static_cast<std::uint32_t>(v));
    put32(out, addr + 4, static_cast<std::uint32_t>(v >> 32));
}

// One quadword: low and high 64 bits.
void put128(std::vector<std::uint8_t>& out, std::uint32_t addr, std::uint64_t lo, std::uint64_t hi) {
    put64(out, addr, lo);
    put64(out, addr + 8, hi);
}

} // namespace

std::vector<std::uint8_t> data_segment() {
    std::vector<std::uint8_t> d(data_end - data_base, 0);
    // SemaParam: count, max_count, init_count, wait_threads, attr, option.
    put32(d, sema_param + 0x00, 0);
    put32(d, sema_param + 0x04, 1);
    put32(d, sema_param + 0x08, 0);
    // ThreadParam: status, func, stack, stack_size, gp_reg, initial_priority, current_priority, attr, option.
    put32(d, thread_param + 0x04, 0x00200580u);
    put32(d, thread_param + 0x08, worker_stack);
    put32(d, thread_param + 0x0C, worker_stack_size);
    put32(d, thread_param + 0x14, worker_priority);
    put32(d, thread_param + 0x18, worker_priority);

    // GIF packet, PACKED mode. An A+D tag (NLOOP 4, NREG 1, REGS = A+D) with FRAME_1, SCISSOR_1, XYOFFSET_1
    // and TEST_1, then a sprite tag (NLOOP 1, EOP, PRE, PRIM = sprite, NREG 3, REGS = RGBAQ, XYZ2, XYZ2).
    std::uint32_t q = gif_packet;
    put128(d, q, 4u | (UINT64_C(1) << 60), 0xE);
    put128(d, q += 16, UINT64_C(10) << 16, 0x4C);                            // FRAME_1: FBW 10
    put128(d, q += 16, (UINT64_C(639) << 16) | (UINT64_C(447) << 48), 0x40); // SCISSOR_1
    put128(d, q += 16, 0, 0x18);                                             // XYOFFSET_1
    put128(d, q += 16, UINT64_C(0x30000), 0x47);                             // TEST_1: ZTE, ZTST always
    put128(d, q += 16,
           1u | (UINT64_C(1) << 15) | (UINT64_C(1) << 46) | (UINT64_C(6) << 47) | (UINT64_C(3) << 60), 0x551);
    put128(d, q += 16, (UINT64_C(0x40) << 32), (UINT64_C(0x80) << 32) | 0x20);  // RGBAQ: R (frame), G, B, A
    put128(d, q += 16, (UINT64_C(64) << 4) | ((UINT64_C(64) << 4) << 32), 0);   // XYZ2 (64, 64)
    put128(d, q += 16, (UINT64_C(256) << 4) | ((UINT64_C(192) << 4) << 32), 0); // XYZ2 (256, 192)
    return d;
}

std::vector<std::uint8_t> disc_file_bytes() {
    std::vector<std::uint8_t> bytes(4096);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(i * 7u + 3u);
    }
    return bytes;
}

} // namespace machine_unit
