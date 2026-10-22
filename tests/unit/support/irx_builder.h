// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "support/mips_asm.h"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Synthetic IRX modules for the interpreted IOP's tests (Docs/milestones/M4-playable.md, session A): a small
// assembler with labels whose output is an IOP relocatable ELF in the shape the loader reads: type 0xFF80, an
// .iopmod program header, one PT_LOAD linked at 0, REL relocations against the image base, import tables in
// the text and export tables in the data. Everything is built from the public formats at test time; no game
// data.
namespace dq8::test {

// Encoders mips_asm.h does not have, from the public MIPS I instruction formats.
namespace iopasm {
using mips::Reg;
constexpr std::uint32_t lb(Reg rt, std::int32_t off, Reg base) {
    return mips::i_type(0x20, base, rt, mips::simm(off));
}
constexpr std::uint32_t lh(Reg rt, std::int32_t off, Reg base) {
    return mips::i_type(0x21, base, rt, mips::simm(off));
}
constexpr std::uint32_t lwl(Reg rt, std::int32_t off, Reg base) {
    return mips::i_type(0x22, base, rt, mips::simm(off));
}
constexpr std::uint32_t lhu(Reg rt, std::int32_t off, Reg base) {
    return mips::i_type(0x25, base, rt, mips::simm(off));
}
constexpr std::uint32_t lwr(Reg rt, std::int32_t off, Reg base) {
    return mips::i_type(0x26, base, rt, mips::simm(off));
}
constexpr std::uint32_t sh(Reg rt, std::int32_t off, Reg base) {
    return mips::i_type(0x29, base, rt, mips::simm(off));
}
constexpr std::uint32_t swl(Reg rt, std::int32_t off, Reg base) {
    return mips::i_type(0x2A, base, rt, mips::simm(off));
}
constexpr std::uint32_t swr(Reg rt, std::int32_t off, Reg base) {
    return mips::i_type(0x2E, base, rt, mips::simm(off));
}
constexpr std::uint32_t addi(Reg rt, Reg rs, std::int32_t imm) {
    return mips::i_type(0x08, rs, rt, mips::simm(imm));
}
constexpr std::uint32_t sltiu(Reg rt, Reg rs, std::int32_t imm) {
    return mips::i_type(0x0B, rs, rt, mips::simm(imm));
}
constexpr std::uint32_t xori(Reg rt, Reg rs, std::uint32_t imm) {
    return mips::i_type(0x0E, rs, rt, imm);
}
constexpr std::uint32_t blez(Reg rs, std::int32_t offset) {
    return mips::i_type(0x06, rs, 0, mips::simm(offset));
}
constexpr std::uint32_t bgtz(Reg rs, std::int32_t offset) {
    return mips::i_type(0x07, rs, 0, mips::simm(offset));
}
constexpr std::uint32_t srl(Reg rd, Reg rt, std::uint32_t sa) {
    return mips::r_type(0, rt, rd, sa, 0x02);
}
constexpr std::uint32_t sra(Reg rd, Reg rt, std::uint32_t sa) {
    return mips::r_type(0, rt, rd, sa, 0x03);
}
constexpr std::uint32_t sllv(Reg rd, Reg rt, Reg rs) {
    return mips::r_type(rs, rt, rd, 0, 0x04);
}
constexpr std::uint32_t srav(Reg rd, Reg rt, Reg rs) {
    return mips::r_type(rs, rt, rd, 0, 0x07);
}
constexpr std::uint32_t mfhi(Reg rd) {
    return mips::r_type(0, 0, rd, 0, 0x10);
}
constexpr std::uint32_t mflo(Reg rd) {
    return mips::r_type(0, 0, rd, 0, 0x12);
}
constexpr std::uint32_t mult(Reg rs, Reg rt) {
    return mips::r_type(rs, rt, 0, 0, 0x18);
}
constexpr std::uint32_t multu(Reg rs, Reg rt) {
    return mips::r_type(rs, rt, 0, 0, 0x19);
}
constexpr std::uint32_t div(Reg rs, Reg rt) {
    return mips::r_type(rs, rt, 0, 0, 0x1A);
}
constexpr std::uint32_t divu(Reg rs, Reg rt) {
    return mips::r_type(rs, rt, 0, 0, 0x1B);
}
constexpr std::uint32_t add(Reg rd, Reg rs, Reg rt) {
    return mips::r_type(rs, rt, rd, 0, 0x20);
}
constexpr std::uint32_t sub(Reg rd, Reg rs, Reg rt) {
    return mips::r_type(rs, rt, rd, 0, 0x22);
}
constexpr std::uint32_t xor_(Reg rd, Reg rs, Reg rt) {
    return mips::r_type(rs, rt, rd, 0, 0x26);
}
constexpr std::uint32_t nor(Reg rd, Reg rs, Reg rt) {
    return mips::r_type(rs, rt, rd, 0, 0x27);
}
constexpr std::uint32_t sltu(Reg rd, Reg rs, Reg rt) {
    return mips::r_type(rs, rt, rd, 0, 0x2B);
}
constexpr std::uint32_t mfc0(Reg rt, std::uint32_t rd) {
    return (0x10u << 26) | ((rt & 31u) << 16) | ((rd & 31u) << 11);
}
} // namespace iopasm

class IrxBuilder {
  public:
    struct Label {
        std::uint32_t id = 0;
    };

    explicit IrxBuilder(std::string name, std::uint16_t version = 0x0101);

    // ---- labels
    Label label();
    void bind(Label l); // to the current text position
    Label here();       // a new label at the current text position

    // ---- text
    std::uint32_t text_size() const { return static_cast<std::uint32_t>(text_.size() * 4); }
    void emit(std::uint32_t word);
    // An import table at the current text position; stub(library, ordinal) names its stubs afterwards.
    void imports(const std::string& library, std::initializer_list<std::uint16_t> ordinals,
                 std::uint16_t version = 0x0101);
    Label stub(std::string_view library, std::uint16_t ordinal) const;
    void jal(Label target); // then a nop
    void j(Label target);   // then a nop
    void la(mips::Reg r, Label target);
    void li(mips::Reg r, std::uint32_t value);
    void beq(mips::Reg rs, mips::Reg rt, Label target); // then a nop
    void bne(mips::Reg rs, mips::Reg rt, Label target); // then a nop
    void b(Label target);                               // then a nop
    // li $a0.. with the arguments, then jal and a nop.
    void call(Label target, std::initializer_list<std::uint32_t> args = {});
    void ret(); // jr $ra, nop
    // A frame saving $ra, $s0 and $s1; leave() restores them and returns.
    void enter(std::uint32_t frame = 32);
    void leave(std::uint32_t frame = 32);
    // sw r, offset(target), through $t9.
    void store(Label target, std::uint32_t offset, mips::Reg r);
    // lw r, offset(target), through $t9, and the load-delay nop.
    void load(mips::Reg r, Label target, std::uint32_t offset = 0);
    // Appends r to a log (a count word, then the entries); clobbers $t7-$t9.
    void append(Label log, mips::Reg r);
    void append_value(Label log, std::uint32_t value);

    // ---- data and bss
    Label data(std::span<const std::uint8_t> bytes, std::uint32_t align = 16);
    Label words(std::initializer_list<std::uint32_t> values);
    Label zeros(std::uint32_t size, std::uint32_t align = 16);
    Label string(std::string_view text);
    // The word at data label `at` + `offset` holds the address of `target` (an R_MIPS_32 relocation).
    void pointer(Label at, std::uint32_t offset, Label target);
    Label exports(const std::string& library, std::initializer_list<Label> functions,
                  std::uint16_t version = 0x0101);
    Label bss(std::uint32_t size, std::uint32_t align = 16);

    // ---- the module
    void entry(Label l) { entry_ = l; }
    // Loader failure tests: an extra relocation of any type at a text offset, another ELF type.
    void raw_relocation(std::uint32_t text_offset, std::uint8_t type);
    void set_elf_type(std::uint16_t type) { elf_type_ = type; }

    std::vector<std::uint8_t> build() const;
    // The image-relative address of a label in the built module.
    std::uint32_t offset_of(Label l) const;

  private:
    enum class Section : std::uint8_t { none, text, data, bss };
    struct LabelInfo {
        Section section = Section::none;
        std::uint32_t offset = 0;
    };
    enum class Fix : std::uint8_t { j26, hi16, lo16, branch16 };
    struct TextFixup {
        std::uint32_t offset = 0;
        Fix kind = Fix::j26;
        Label target;
    };
    struct DataFixup {
        std::uint32_t offset = 0;
        Label target;
    };
    struct Layout {
        std::uint32_t data_base = 0;
        std::uint32_t bss_base = 0;
        std::uint32_t mem_size = 0;
    };

    Layout layout() const;
    std::uint32_t address(Label l, const Layout& layout) const;

    std::string name_;
    std::uint16_t version_;
    std::uint16_t elf_type_ = 0xFF80;
    std::vector<std::uint32_t> text_;
    std::vector<std::uint8_t> data_;
    std::uint32_t bss_size_ = 0;
    std::vector<LabelInfo> labels_{LabelInfo{}};
    std::vector<TextFixup> text_fixups_;
    std::vector<DataFixup> data_fixups_;
    std::vector<std::pair<std::uint32_t, std::uint8_t>> raw_;
    std::map<std::pair<std::string, std::uint16_t>, Label> stubs_;
    Label entry_;
};

} // namespace dq8::test
