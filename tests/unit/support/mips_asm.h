// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// constexpr encoders for the MIPS I/II instructions our synthetic fixtures need (unit tests and the e2e
// fixture generator). Every word is assembled here from the public instruction formats; nothing is copied
// from a game. Branch offsets are in instructions relative to the delay slot, as the hardware computes them;
// jump targets are absolute byte addresses inside the current 256 MiB region.
namespace dq8::test::mips {

// clang-format off
enum Reg : std::uint32_t {
    zero = 0, at = 1, v0 = 2, v1 = 3, a0 = 4, a1 = 5, a2 = 6, a3 = 7,
    t0 = 8, t1 = 9, t2 = 10, t3 = 11, t4 = 12, t5 = 13, t6 = 14, t7 = 15,
    s0 = 16, s1 = 17, s2 = 18, s3 = 19, s4 = 20, s5 = 21, s6 = 22, s7 = 23,
    t8 = 24, t9 = 25, k0 = 26, k1 = 27, gp = 28, sp = 29, fp = 30, ra = 31,
};
// clang-format on

constexpr std::uint32_t r_type(std::uint32_t rs, std::uint32_t rt, std::uint32_t rd, std::uint32_t shamt,
                               std::uint32_t funct) {
    return ((rs & 31u) << 21) | ((rt & 31u) << 16) | ((rd & 31u) << 11) | ((shamt & 31u) << 6) |
           (funct & 63u);
}
constexpr std::uint32_t i_type(std::uint32_t op, std::uint32_t rs, std::uint32_t rt, std::uint32_t imm) {
    return ((op & 63u) << 26) | ((rs & 31u) << 21) | ((rt & 31u) << 16) | (imm & 0xFFFFu);
}
constexpr std::uint32_t j_type(std::uint32_t op, std::uint32_t target) {
    return ((op & 63u) << 26) | ((target >> 2) & 0x03FFFFFFu);
}
// A signed 16-bit immediate as the instruction stores it.
constexpr std::uint32_t simm(std::int32_t v) {
    return static_cast<std::uint32_t>(v) & 0xFFFFu;
}

// SPECIAL (opcode 0)
constexpr std::uint32_t nop() {
    return 0;
}
constexpr std::uint32_t sll(Reg rd, Reg rt, std::uint32_t sa) {
    return r_type(0, rt, rd, sa, 0x00);
}
constexpr std::uint32_t jr(Reg rs) {
    return r_type(rs, 0, 0, 0, 0x08);
}
constexpr std::uint32_t jalr(Reg rd, Reg rs) {
    return r_type(rs, 0, rd, 0, 0x09);
}
constexpr std::uint32_t syscall(std::uint32_t code = 0) {
    return ((code & 0xFFFFFu) << 6) | 0x0C;
}
constexpr std::uint32_t break_(std::uint32_t code = 0) {
    return ((code & 0xFFFFFu) << 6) | 0x0D;
}
constexpr std::uint32_t addu(Reg rd, Reg rs, Reg rt) {
    return r_type(rs, rt, rd, 0, 0x21);
}
constexpr std::uint32_t subu(Reg rd, Reg rs, Reg rt) {
    return r_type(rs, rt, rd, 0, 0x23);
}
constexpr std::uint32_t and_(Reg rd, Reg rs, Reg rt) {
    return r_type(rs, rt, rd, 0, 0x24);
}
constexpr std::uint32_t or_(Reg rd, Reg rs, Reg rt) {
    return r_type(rs, rt, rd, 0, 0x25);
}
constexpr std::uint32_t slt(Reg rd, Reg rs, Reg rt) {
    return r_type(rs, rt, rd, 0, 0x2A);
}
constexpr std::uint32_t move(Reg rd, Reg rs) {
    return addu(rd, rs, zero);
}

// REGIMM (opcode 1): the link forms write $ra and continue at the branch's address + 8 on return.
constexpr std::uint32_t bltz(Reg rs, std::int32_t offset) {
    return i_type(0x01, rs, 0x00, simm(offset));
}
constexpr std::uint32_t bgez(Reg rs, std::int32_t offset) {
    return i_type(0x01, rs, 0x01, simm(offset));
}
constexpr std::uint32_t bltzal(Reg rs, std::int32_t offset) {
    return i_type(0x01, rs, 0x10, simm(offset));
}
constexpr std::uint32_t bgezal(Reg rs, std::int32_t offset) {
    return i_type(0x01, rs, 0x11, simm(offset));
}
constexpr std::uint32_t bal(std::int32_t offset) {
    return bgezal(zero, offset);
}

// Jumps
constexpr std::uint32_t j(std::uint32_t target) {
    return j_type(0x02, target);
}
constexpr std::uint32_t jal(std::uint32_t target) {
    return j_type(0x03, target);
}

// Branches and immediates
constexpr std::uint32_t beq(Reg rs, Reg rt, std::int32_t offset) {
    return i_type(0x04, rs, rt, simm(offset));
}
constexpr std::uint32_t bne(Reg rs, Reg rt, std::int32_t offset) {
    return i_type(0x05, rs, rt, simm(offset));
}
constexpr std::uint32_t b(std::int32_t offset) {
    return beq(zero, zero, offset);
}
constexpr std::uint32_t addiu(Reg rt, Reg rs, std::int32_t imm) {
    return i_type(0x09, rs, rt, simm(imm));
}
constexpr std::uint32_t slti(Reg rt, Reg rs, std::int32_t imm) {
    return i_type(0x0A, rs, rt, simm(imm));
}
constexpr std::uint32_t andi(Reg rt, Reg rs, std::uint32_t imm) {
    return i_type(0x0C, rs, rt, imm);
}
constexpr std::uint32_t ori(Reg rt, Reg rs, std::uint32_t imm) {
    return i_type(0x0D, rs, rt, imm);
}
constexpr std::uint32_t lui(Reg rt, std::uint32_t imm) {
    return i_type(0x0F, 0, rt, imm);
}
constexpr std::uint32_t li(Reg rt, std::int32_t imm) {
    return addiu(rt, zero, imm);
}

// Loads and stores: base register plus signed 16-bit offset.
constexpr std::uint32_t lw(Reg rt, std::int32_t offset, Reg base) {
    return i_type(0x23, base, rt, simm(offset));
}
constexpr std::uint32_t sw(Reg rt, std::int32_t offset, Reg base) {
    return i_type(0x2B, base, rt, simm(offset));
}
constexpr std::uint32_t lbu(Reg rt, std::int32_t offset, Reg base) {
    return i_type(0x24, base, rt, simm(offset));
}
constexpr std::uint32_t sb(Reg rt, std::int32_t offset, Reg base) {
    return i_type(0x28, base, rt, simm(offset));
}

// The branch offset (in instructions) from a branch at `branch_address` to `target`.
constexpr std::int32_t branch_offset(std::uint32_t branch_address, std::uint32_t target) {
    return (static_cast<std::int32_t>(target) - static_cast<std::int32_t>(branch_address + 4)) / 4;
}

// A tiny program builder: words appended in order from a base address, little-endian on output.
class Program {
  public:
    explicit constexpr Program(std::uint32_t base) : base_(base) {}

    constexpr std::uint32_t base() const { return base_; }
    // The address the next emitted word will have.
    constexpr std::uint32_t here() const { return base_ + static_cast<std::uint32_t>(words_.size() * 4); }
    constexpr std::uint32_t emit(std::uint32_t word) {
        const std::uint32_t address = here();
        words_.push_back(word);
        return address;
    }
    // Replaces the word at `address` (for forward branches and calls).
    constexpr void patch(std::uint32_t address, std::uint32_t word) { words_[(address - base_) / 4] = word; }
    constexpr const std::vector<std::uint32_t>& words() const { return words_; }
    constexpr std::size_t size_bytes() const { return words_.size() * 4; }

    std::vector<std::uint8_t> bytes() const {
        std::vector<std::uint8_t> out;
        out.reserve(words_.size() * 4);
        for (const std::uint32_t w : words_) {
            for (int i = 0; i < 4; ++i) {
                out.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
            }
        }
        return out;
    }

  private:
    std::uint32_t base_;
    std::vector<std::uint32_t> words_;
};

} // namespace dq8::test::mips
