// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/elf_headers.h"
#include "dq8/core/elf_writer.h"
#include "dq8/core/error.h"
#include "dq8/core/overlay_elf.h"
#include "support/mips_asm.h"
#include "support/mwo3_builder.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using dq8::core::ElfSection;
using dq8::core::ElfWriter;
using dq8::core::Error;
using dq8::core::ErrorCode;
namespace core = dq8::core;
namespace m = dq8::test::mips;

namespace {

std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint32_t>(b.at(at)) | (static_cast<std::uint32_t>(b.at(at + 1)) << 8) |
           (static_cast<std::uint32_t>(b.at(at + 2)) << 16) |
           (static_cast<std::uint32_t>(b.at(at + 3)) << 24);
}
std::uint16_t le16(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint16_t>(b.at(at) | (b.at(at + 1) << 8));
}

// The section header table, read independently of the writer (elf_headers.h reads program headers only).
struct Shdr {
    std::string name;
    std::uint32_t type, flags, addr, offset, size, align;
};

std::vector<Shdr> sections_of(const std::vector<std::uint8_t>& elf) {
    const std::uint32_t shoff = le32(elf, 32);
    const std::uint16_t shentsize = le16(elf, 46);
    const std::uint16_t shnum = le16(elf, 48);
    const std::uint16_t shstrndx = le16(elf, 50);
    REQUIRE(shentsize == 40);
    REQUIRE(shstrndx < shnum);
    auto raw = [&](std::size_t i, std::size_t field) { return le32(elf, shoff + i * shentsize + field); };
    const std::uint32_t strtab = raw(shstrndx, 16);
    std::vector<Shdr> out;
    for (std::size_t i = 0; i < shnum; ++i) {
        Shdr s{};
        for (std::size_t c = strtab + raw(i, 0); c < elf.size() && elf[c] != 0; ++c) {
            s.name.push_back(static_cast<char>(elf[c]));
        }
        s.type = raw(i, 4);
        s.flags = raw(i, 8);
        s.addr = raw(i, 12);
        s.offset = raw(i, 16);
        s.size = raw(i, 20);
        s.align = raw(i, 32);
        out.push_back(s);
    }
    return out;
}

ElfSection progbits(std::string name, std::uint32_t address, std::vector<std::uint8_t> data,
                    std::uint32_t flags = core::elf_shf_alloc | core::elf_shf_execinstr) {
    ElfSection s;
    s.name = std::move(name);
    s.type = core::elf_sht_progbits;
    s.flags = flags;
    s.address = address;
    s.data = std::move(data);
    return s;
}

ElfSection nobits(std::string name, std::uint32_t address, std::uint32_t size) {
    ElfSection s;
    s.name = std::move(name);
    s.type = core::elf_sht_nobits;
    s.flags = core::elf_shf_alloc | core::elf_shf_write;
    s.address = address;
    s.nobits_size = size;
    return s;
}

ErrorCode build_error(const ElfWriter& w, const std::string& fragment) {
    try {
        (void)w.build();
    } catch (const Error& e) {
        CHECK_THAT(e.what(), ContainsSubstring(fragment));
        return e.code();
    }
    FAIL("expected build() to refuse the layout");
    return ErrorCode::io_open;
}

} // namespace

TEST_CASE("ElfWriter writes a MIPS executable the header parser reads back", "[core][elf-writer]") {
    m::Program prog(0x00100000);
    prog.emit(m::lui(m::a0, 0x1234));
    prog.emit(m::jr(m::ra));
    prog.emit(m::nop());
    ElfWriter w;
    w.set_entry(0x00100000).set_flags(0x20924001u);
    w.add_section(progbits(".text", 0x00100000, prog.bytes()));
    w.add_section(progbits(".data", 0x00100100, std::vector<std::uint8_t>(8, 0xAB),
                           core::elf_shf_alloc | core::elf_shf_write));
    w.add_section(nobits(".bss", 0x00100200, 0x40));
    const std::vector<std::uint8_t> elf = w.build();

    const core::ElfHeaders h = core::parse_elf_headers(elf);
    CHECK(h.type == core::elf_et_exec);
    CHECK(h.machine == core::elf_em_mips);
    CHECK(h.entry == 0x00100000);
    CHECK(h.flags == 0x20924001u);
    const auto loads = h.load_segments();
    REQUIRE(loads.size() == 3);
    CHECK(loads[0]->vaddr == 0x00100000);
    CHECK(loads[0]->filesz == 12);
    CHECK(loads[0]->memsz == 12);
    CHECK(loads[0]->flags == (core::elf_pf_r | core::elf_pf_x));
    CHECK(loads[1]->vaddr == 0x00100100);
    CHECK(loads[1]->flags == (core::elf_pf_r | core::elf_pf_w));
    CHECK(loads[2]->vaddr == 0x00100200);
    CHECK(loads[2]->filesz == 0);
    CHECK(loads[2]->memsz == 0x40);
    for (const core::ElfSegment* s : loads) {
        CHECK(s->offset % s->align == s->vaddr % s->align);
    }
    // The text bytes sit at the PT_LOAD's offset.
    CHECK(le32(elf, loads[0]->offset) == m::lui(m::a0, 0x1234));
    CHECK(le32(elf, loads[0]->offset + 4) == m::jr(m::ra));

    const std::vector<Shdr> sh = sections_of(elf);
    REQUIRE(sh.size() == 5); // null, .text, .data, .bss, .shstrtab
    CHECK(sh[0].type == 0);
    CHECK(sh[1].name == ".text");
    CHECK(sh[1].flags == (core::elf_shf_alloc | core::elf_shf_execinstr));
    CHECK(sh[1].offset == loads[0]->offset);
    CHECK(sh[2].name == ".data");
    CHECK(sh[3].name == ".bss");
    CHECK(sh[3].type == core::elf_sht_nobits);
    CHECK(sh[3].size == 0x40);
    CHECK(sh[4].name == ".shstrtab");
    CHECK(sh[4].type == core::elf_sht_strtab);
}

TEST_CASE("ElfWriter sorts program headers by address and aligns file offsets", "[core][elf-writer]") {
    ElfWriter w;
    ElfSection high = progbits(".high", 0x00200040, std::vector<std::uint8_t>(5, 1));
    high.align = 64;
    w.add_section(std::move(high));
    w.add_section(progbits(".low", 0x00100000, std::vector<std::uint8_t>(3, 2)));
    w.add_section(progbits(".note", 0, std::vector<std::uint8_t>(4, 3), 0)); // not allocated: no PT_LOAD
    const auto elf = w.build();
    const auto h = core::parse_elf_headers(elf);
    REQUIRE(h.segments.size() == 2);
    CHECK(h.segments[0].vaddr == 0x00100000);
    CHECK(h.segments[1].vaddr == 0x00200040);
    CHECK(h.segments[1].offset % 64 == 0);
    CHECK(h.entry == 0);
}

TEST_CASE("ElfWriter refuses inconsistent layouts", "[core][elf-writer]") {
    SECTION("overlapping allocated sections") {
        ElfWriter w;
        w.add_section(progbits(".a", 0x1000, std::vector<std::uint8_t>(0x20)));
        w.add_section(nobits(".b", 0x1010, 0x10));
        CHECK(build_error(w, "overlap") == ErrorCode::elf_layout);
    }
    SECTION("a misaligned address") {
        ElfWriter w;
        ElfSection s = progbits(".a", 0x1002, std::vector<std::uint8_t>(4));
        w.add_section(std::move(s));
        CHECK(build_error(w, "not aligned") == ErrorCode::elf_layout);
    }
    SECTION("an alignment that is not a power of two") {
        ElfWriter w;
        ElfSection s = progbits(".a", 0x1000, std::vector<std::uint8_t>(4));
        s.align = 12;
        w.add_section(std::move(s));
        CHECK(build_error(w, "power of two") == ErrorCode::elf_layout);
    }
    SECTION("duplicate and reserved names") {
        ElfWriter w;
        w.add_section(progbits(".a", 0x1000, std::vector<std::uint8_t>(4)));
        w.add_section(progbits(".a", 0x2000, std::vector<std::uint8_t>(4)));
        CHECK(build_error(w, "twice") == ErrorCode::elf_layout);
        ElfWriter r;
        r.add_section(progbits(".shstrtab", 0x1000, std::vector<std::uint8_t>(4)));
        CHECK(build_error(r, "reserved") == ErrorCode::elf_layout);
    }
    SECTION("NOBITS with data") {
        ElfWriter w;
        ElfSection s = nobits(".bss", 0x1000, 0);
        s.data.assign(4, 0);
        w.add_section(std::move(s));
        CHECK(build_error(w, "NOBITS") == ErrorCode::elf_layout);
    }
    SECTION("a section that wraps past 4 GiB") {
        ElfWriter w;
        w.add_section(nobits(".bss", 0xFFFFFF00u, 0x200));
        CHECK(build_error(w, "32-bit") == ErrorCode::elf_layout);
    }
}

TEST_CASE("make_overlay_unit_elf maps the whole container at its load address", "[core][elf-writer][mwo3]") {
    dq8::test::Mwo3Spec spec;
    spec.load_address = 0x00200000;
    spec.text_size = 0x100;
    spec.data_size = 0x40;
    spec.bss_size = 0x30;
    const std::vector<std::uint8_t> container = dq8::test::build_mwo3(spec);
    const core::OverlayUnitElf unit =
        core::make_overlay_unit_elf(container, 0x20924001u, core::Mwo3Window{0x00200000, 0x00210000});

    CHECK(unit.header.overlay_id == spec.overlay_id);
    const auto h = core::parse_elf_headers(unit.elf);
    CHECK(h.entry == 0);
    CHECK(h.flags == 0x20924001u);
    CHECK(h.machine == core::elf_em_mips);
    const auto loads = h.load_segments();
    REQUIRE(loads.size() == 2);
    CHECK(loads[0]->vaddr == 0x00200000);
    CHECK(loads[0]->filesz == container.size());
    CHECK(loads[0]->memsz == container.size());
    CHECK(loads[0]->flags == (core::elf_pf_r | core::elf_pf_w | core::elf_pf_x));
    CHECK(loads[1]->vaddr == 0x00200000 + container.size());
    CHECK(loads[1]->filesz == 0);
    CHECK(loads[1]->memsz == 0x30);
    // Header included, byte for byte.
    CHECK(std::equal(container.begin(), container.end(),
                     unit.elf.begin() + static_cast<std::ptrdiff_t>(loads[0]->offset)));

    const std::vector<Shdr> sh = sections_of(unit.elf);
    REQUIRE(sh.size() == 4);
    CHECK(sh[1].name == ".text");
    CHECK(sh[1].type == core::elf_sht_progbits);
    CHECK(sh[1].flags == (core::elf_shf_alloc | core::elf_shf_execinstr | core::elf_shf_write));
    CHECK(sh[1].addr == 0x00200000);
    CHECK(sh[1].size == container.size());
    CHECK(sh[2].name == ".bss");
    CHECK(sh[2].type == core::elf_sht_nobits);
    CHECK(sh[2].addr == spec.load_address + container.size());
    CHECK(std::none_of(sh.begin(), sh.end(), [](const Shdr& s) { return s.type == 2; })); // no SHT_SYMTAB
}

TEST_CASE("make_overlay_unit_elf omits .bss when the container has none", "[core][elf-writer][mwo3]") {
    dq8::test::Mwo3Spec spec;
    spec.bss_size = 0;
    const auto unit = core::make_overlay_unit_elf(dq8::test::build_mwo3(spec), 0);
    CHECK(core::parse_elf_headers(unit.elf).load_segments().size() == 1);
    CHECK(sections_of(unit.elf).size() == 3);
}

TEST_CASE("make_overlay_unit_elf passes container errors through", "[core][elf-writer][mwo3]") {
    dq8::test::Mwo3Spec spec;
    auto container = dq8::test::build_mwo3(spec);
    container.pop_back();
    try {
        (void)core::make_overlay_unit_elf(container, 0);
        FAIL("expected the truncated container to be refused");
    } catch (const Error& e) {
        CHECK(e.code() == ErrorCode::mwo3_size);
    }
}
