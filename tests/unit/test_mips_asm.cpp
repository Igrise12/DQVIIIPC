// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "support/mips_asm.h"

#include <catch2/catch_test_macros.hpp>

namespace m = dq8::test::mips;

// Expected words are hand-assembled from the MIPS instruction formats (opcode, rs, rt, rd, shamt, funct).
static_assert(m::nop() == 0);
static_assert(m::lui(m::a0, 0x1234) == 0x3C041234u);
static_assert(m::jr(m::ra) == 0x03E00008u);

TEST_CASE("the encoders produce the architectural instruction words", "[test-support][mips]") {
    CHECK(m::lui(m::a0, 0x1234) == 0x3C041234u);
    CHECK(m::ori(m::a0, m::a0, 0x5678) == 0x34845678u);
    CHECK(m::addiu(m::v1, m::zero, 0x3C) == 0x2403003Cu);
    CHECK(m::li(m::v1, 0x3C) == 0x2403003Cu);
    CHECK(m::addiu(m::v0, m::a0, 1) == 0x24820001u);
    CHECK(m::addiu(m::sp, m::sp, -16) == 0x27BDFFF0u);
    CHECK(m::sw(m::ra, 12, m::sp) == 0xAFBF000Cu);
    CHECK(m::lw(m::ra, 12, m::sp) == 0x8FBF000Cu);
    CHECK(m::jr(m::ra) == 0x03E00008u);
    CHECK(m::jalr(m::ra, m::t9) == 0x0320F809u);
    CHECK(m::move(m::v0, m::a0) == 0x00801021u);
    CHECK(m::syscall() == 0x0000000Cu);
    CHECK(m::syscall(0x3C) == ((0x3Cu << 6) | 0x0Cu));
    CHECK(m::sll(m::zero, m::zero, 0) == m::nop());
}

TEST_CASE("jumps keep the target's word index, branches count from the delay slot", "[test-support][mips]") {
    CHECK(m::jal(0x00100040) == 0x0C040010u);
    CHECK(m::j(0x00100040) == 0x08040010u);
    CHECK(m::bgezal(m::a0, 3) == 0x04910003u);
    CHECK(m::bltzal(m::a0, 3) == 0x04900003u);
    CHECK(m::bal(2) == 0x04110002u);
    CHECK(m::beq(m::zero, m::zero, -1) == 0x1000FFFFu);
    CHECK(m::bne(m::a0, m::v0, 4) == 0x14820004u);
    CHECK(m::branch_offset(0x00100010, 0x00100020) == 3);
    CHECK(m::branch_offset(0x00100010, 0x00100010) == -1);
}

TEST_CASE("Program lays words out from its base and patches forward references", "[test-support][mips]") {
    m::Program p(0x00100000);
    p.emit(m::lui(m::a0, 0x1234));
    const std::uint32_t call = p.emit(m::nop());
    p.emit(m::nop());
    const std::uint32_t helper = p.emit(m::addiu(m::v0, m::a0, 1));
    p.emit(m::jr(m::ra));
    p.emit(m::nop());
    p.patch(call, m::jal(helper));

    CHECK(p.here() == 0x00100018);
    CHECK(helper == 0x0010000C);
    REQUIRE(p.words().size() == 6);
    CHECK(p.words()[1] == m::jal(0x0010000C));
    const auto bytes = p.bytes();
    REQUIRE(bytes.size() == 24);
    // Little-endian: the low byte of `lui a0,0x1234` comes first.
    CHECK(bytes[0] == 0x34);
    CHECK(bytes[1] == 0x12);
    CHECK(bytes[2] == 0x04);
    CHECK(bytes[3] == 0x3C);
}
