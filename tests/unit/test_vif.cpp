// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// VIF0/VIF1 (session B "Done when": UNPACK formats and masks), beyond the cases ported from upstream in
// test_mmio_devices.cpp.
#include "runtime/ee/guest_stop.h"
#include "runtime/machine/interrupts.h"
#include "support/ee_rig.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <vector>

using namespace dq8::rt;
using dq8::test::EeRig;
using dq8::test::put32;
using dq8::test::vif_code;

namespace {

std::array<std::uint32_t, 4> vu1_qword(EeRig& rig, unsigned index) {
    std::array<std::uint32_t, 4> q{};
    std::memcpy(q.data(), rig.vu().data(1).data() + index * 16u, 16);
    return q;
}

void push_words(Vif& vif, std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint8_t> bytes;
    for (const std::uint32_t w : words) {
        put32(bytes, w);
    }
    vif.push(bytes);
}

void push_bytes(Vif& vif, std::uint32_t code, std::initializer_list<std::uint8_t> data) {
    std::vector<std::uint8_t> bytes;
    put32(bytes, code);
    bytes.insert(bytes.end(), data);
    while (bytes.size() % 4 != 0) {
        bytes.push_back(0);
    }
    vif.push(bytes);
}

using Q = std::array<std::uint32_t, 4>;

} // namespace

TEST_CASE("vif: every UNPACK format, signed and unsigned", "[vif]") {
    EeRig rig;
    Vif& vif = rig.vif1();
    const std::uint32_t u = 0x4000; // IMMEDIATE bit 14: unsigned

    // S-32 / S-16 / S-8: one value in every lane.
    push_words(vif, {vif_code(0x60, 1, 0), 0x11223344u});
    CHECK(vu1_qword(rig, 0) == Q{0x11223344u, 0x11223344u, 0x11223344u, 0x11223344u});
    push_bytes(vif, vif_code(0x61, 1, 1), {0x01, 0x80});
    CHECK(vu1_qword(rig, 1) == Q{0xFFFF8001u, 0xFFFF8001u, 0xFFFF8001u, 0xFFFF8001u});
    push_bytes(vif, vif_code(0x61, 1, 2 | u), {0x01, 0x80});
    CHECK(vu1_qword(rig, 2) == Q{0x8001u, 0x8001u, 0x8001u, 0x8001u});
    push_bytes(vif, vif_code(0x62, 1, 3), {0x80});
    CHECK(vu1_qword(rig, 3) == Q{0xFFFFFF80u, 0xFFFFFF80u, 0xFFFFFF80u, 0xFFFFFF80u});
    push_bytes(vif, vif_code(0x62, 1, 4 | u), {0x80});
    CHECK(vu1_qword(rig, 4) == Q{0x80u, 0x80u, 0x80u, 0x80u});

    // V2: x y x y (PCSX2: the hardware repeats the pair).
    push_words(vif, {vif_code(0x64, 1, 5), 0xAu, 0xBu});
    CHECK(vu1_qword(rig, 5) == Q{0xAu, 0xBu, 0xAu, 0xBu});
    push_bytes(vif, vif_code(0x65, 1, 6), {0xFF, 0xFF, 0x02, 0x00});
    CHECK(vu1_qword(rig, 6) == Q{0xFFFFFFFFu, 2u, 0xFFFFFFFFu, 2u});
    push_bytes(vif, vif_code(0x66, 1, 7 | u), {0xFE, 0x03});
    CHECK(vu1_qword(rig, 7) == Q{0xFEu, 3u, 0xFEu, 3u});

    // V3: W takes the next component in the stream (the next vector's X), 0 past the end of the data.
    push_words(vif, {vif_code(0x68, 2, 8), 1u, 2u, 3u, 4u, 5u, 6u});
    CHECK(vu1_qword(rig, 8) == Q{1u, 2u, 3u, 4u});
    CHECK(vu1_qword(rig, 9) == Q{4u, 5u, 6u, 0u});
    push_bytes(vif, vif_code(0x69, 1, 10), {0x00, 0x80, 0x01, 0x00, 0xFF, 0x7F});
    CHECK(vu1_qword(rig, 10) == Q{0xFFFF8000u, 1u, 0x7FFFu, 0u});
    push_bytes(vif, vif_code(0x6A, 1, 11 | u), {0x81, 0x02, 0x83});
    CHECK(vu1_qword(rig, 11) == Q{0x81u, 2u, 0x83u, 0u});

    // V4-32 / V4-16 / V4-8.
    push_words(vif, {vif_code(0x6C, 1, 12), 9u, 8u, 7u, 6u});
    CHECK(vu1_qword(rig, 12) == Q{9u, 8u, 7u, 6u});
    push_bytes(vif, vif_code(0x6D, 1, 13), {0x01, 0x00, 0xFF, 0xFF, 0x00, 0x80, 0xFF, 0x7F});
    CHECK(vu1_qword(rig, 13) == Q{1u, 0xFFFFFFFFu, 0xFFFF8000u, 0x7FFFu});
    push_bytes(vif, vif_code(0x6E, 1, 14), {0x7F, 0x80, 0xFF, 0x00});
    CHECK(vu1_qword(rig, 14) == Q{0x7Fu, 0xFFFFFF80u, 0xFFFFFFFFu, 0u});
    push_bytes(vif, vif_code(0x6E, 1, 15 | u), {0x7F, 0x80, 0xFF, 0x00});
    CHECK(vu1_qword(rig, 15) == Q{0x7Fu, 0x80u, 0xFFu, 0u});

    // V4-5: a 5-5-5-1 colour, each channel at the top of its byte.
    push_bytes(vif, vif_code(0x6F, 2, 16), {0xFF, 0xFF, 0x21, 0x84});
    CHECK(vu1_qword(rig, 16) == Q{0xF8u, 0xF8u, 0xF8u, 0x80u});
    CHECK(vu1_qword(rig, 17) == Q{8u, 8u, 8u, 0x80u});

    // The invalid V*-5 formats stop (ERR.ME1 clear) or are ignored (set).
    CHECK_THROWS_AS(push_words(vif, {vif_code(0x63, 1, 0)}), GuestStop);
}

TEST_CASE("vif: masks select per write cycle row; MODE 3 loads the row", "[vif]") {
    EeRig rig;
    Vif& vif = rig.vif1();
    std::memset(rig.vu().data(1).data() + 3 * 16, 0x5A, 16); // the protected row-3 target
    push_words(vif, {vif_code(0x01, 0, (4u << 8) | 4u),      // STCYCL CL 4 WL 4
                     vif_code(0x20, 0, 0),
                     0xFFAA5500u, // rows: data, row, col, protect
                     vif_code(0x30, 0, 0),
                     0x10u,
                     0x20u,
                     0x30u,
                     0x40u, // STROW
                     vif_code(0x31, 0, 0),
                     0xC0u,
                     0xC1u,
                     0xC2u,
                     0xC3u,                // STCOL
                     vif_code(0x7C, 4, 0), // masked V4-32, 4 vectors
                     1u,
                     2u,
                     3u,
                     4u,
                     5u,
                     6u,
                     7u,
                     8u,
                     9u,
                     10u,
                     11u,
                     12u,
                     13u,
                     14u,
                     15u,
                     16u});
    CHECK(vu1_qword(rig, 0) == Q{1u, 2u, 3u, 4u});
    CHECK(vu1_qword(rig, 1) == Q{0x10u, 0x20u, 0x30u, 0x40u});
    CHECK(vu1_qword(rig, 2) == Q{0xC2u, 0xC2u, 0xC2u, 0xC2u});
    CHECK(vu1_qword(rig, 3) == Q{0x5A5A5A5Au, 0x5A5A5A5Au, 0x5A5A5A5Au, 0x5A5A5A5Au});

    push_words(vif, {vif_code(0x05, 0, 3), vif_code(0x6C, 1, 8), 7u, 8u, 9u, 10u});
    CHECK(vu1_qword(rig, 8) == Q{7u, 8u, 9u, 10u});
    CHECK(vif.registers().row == std::array<std::uint32_t, 4>{7u, 8u, 9u, 10u});

    // V4-5 ignores MODE.
    push_words(vif, {vif_code(0x05, 0, 1)});
    push_bytes(vif, vif_code(0x6F, 1, 9), {0x00, 0x00});
    CHECK(vu1_qword(rig, 9) == Q{0u, 0u, 0u, 0u});
}

TEST_CASE("vif: commands and their data may be split anywhere across pushes", "[vif]") {
    EeRig rig;
    Vif& vif = rig.vif1();
    std::vector<std::uint8_t> bytes;
    put32(bytes, vif_code(0x6C, 2, 0x20));
    for (std::uint32_t i = 0; i < 8; ++i) {
        put32(bytes, 0x100u + i);
    }
    put32(bytes, vif_code(0x07, 0, 0x77)); // MARK
    vif.push(std::span(bytes).subspan(0, 8));
    CHECK((rig.r32(0x10003C00u) & Vif::stat_vps_waiting) != 0u); // waiting for data
    vif.push(std::span(bytes).subspan(8, 20));
    vif.push(std::span(bytes).subspan(28));
    CHECK(vu1_qword(rig, 0x20) == Q{0x100u, 0x101u, 0x102u, 0x103u});
    CHECK(vu1_qword(rig, 0x21) == Q{0x104u, 0x105u, 0x106u, 0x107u});
    CHECK(vif.registers().mark == 0x77u);
    CHECK((rig.r32(0x10003C00u) & Vif::stat_vps_waiting) == 0u);
}

TEST_CASE("vif: the i bit stalls after its command; ERR.MII masks it", "[vif]") {
    EeRig rig;
    Vif& vif = rig.vif1();
    push_words(vif, {vif_code(0x00, 0, 0, true), vif_code(0x05, 0, 2), 0, 0});
    CHECK(vif.stalled());
    CHECK((rig.r32(0x10003C00u) & (Vif::stat_vis | Vif::stat_int)) == (Vif::stat_vis | Vif::stat_int));
    CHECK(((rig.r32(0x10003C00u) >> 24) & 0x1Fu) == 1u); // FQC: the kept quadword
    CHECK(vif.registers().mode == 0u);
    rig.w32(0x10003C10u, 0x8u); // STC
    CHECK(vif.registers().mode == 2u);
    CHECK_FALSE(vif.stalled());

    rig.machine.interrupts().write_intc_stat(0xFFFFu);
    rig.w32(0x10003C20u, 1u); // ERR.MII
    push_words(vif, {vif_code(0x00, 0, 0, true), vif_code(0x05, 0, 1)});
    CHECK_FALSE(vif.stalled());
    CHECK(vif.registers().mode == 1u);
    CHECK(rig.machine.interrupts().intc_stat() == 0u);

    // FBRST.FBK stalls until STC as well.
    rig.w32(0x10003C10u, 0x2u);
    push_words(vif, {vif_code(0x05, 0, 3)});
    CHECK(vif.registers().mode == 1u);
    rig.w32(0x10003C10u, 0x8u);
    CHECK(vif.registers().mode == 3u);
}

TEST_CASE("vif: invalid VIFcodes stop unless ERR.ME1 masks them", "[vif]") {
    EeRig rig;
    CHECK_THROWS_AS(push_words(rig.vif1(), {vif_code(0x08, 0, 0)}), GuestStop);
    EeRig masked;
    masked.w32(0x10003C20u, 4u); // ERR.ME1
    push_words(masked.vif1(), {vif_code(0x08, 0, 0), vif_code(0x07, 0, 5)});
    CHECK(masked.vif1().registers().mark == 5u);
    // VIF1-only commands are invalid on VIF0.
    EeRig vif0;
    CHECK_THROWS_AS(push_words(vif0.vif0(), {vif_code(0x50, 0, 1)}), GuestStop);
}

TEST_CASE("vif: VIF0 uploads and runs a VU0 micro program", "[vif][vu]") {
    EeRig rig;
    Vif& vif = rig.vif0();
    // IADDIU vi1, vi0, 7 with the E bit on its upper NOP, then a NOP pair for the delay slot.
    const std::uint32_t iaddiu = (0x08u << 25) | (1u << 16) | 7u;
    push_words(vif, {vif_code(0x4A, 2, 0), iaddiu, 0x000002FFu | (1u << 30), 0u, 0x000002FFu,
                     vif_code(0x6C, 1, 2), 1u, 2u, 3u, 4u, // UNPACK into VU0 data
                     vif_code(0x14, 0, 0)});               // MSCAL 0
    CHECK(rig.vu().interpreter(0).state().vi[1] == 7);
    CHECK(rig.vu().stats(0).programs == 1u);
    std::uint32_t x = 0;
    std::memcpy(&x, rig.vu().data(0).data() + 32, 4);
    CHECK(x == 1u);
    // Through MMIO: VU0 data at 0x11004000.
    CHECK(rig.r32(0x11004000u + 32u) == 1u);
}

TEST_CASE("vif: a VU program that never ends stops the machine", "[vif][vu]") {
    EeRig rig;
    rig.vu().set_max_program_cycles(1u << 16);
    // B -1: an endless loop with no E bit.
    const std::uint32_t branch_self = (0x20u << 25) | 0x7FFu;
    push_words(rig.vif1(), {vif_code(0x4A, 2, 0), branch_self, 0x000002FFu, 0u, 0x000002FFu});
    CHECK_THROWS_AS(push_words(rig.vif1(), {vif_code(0x14, 0, 0)}), GuestStop);
}
