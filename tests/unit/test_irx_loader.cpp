// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/iop_memory.h"
#include "runtime/iop/loader/iop_libraries.h"
#include "runtime/iop/loader/irx_image.h"
#include "support/irx_builder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rt = dq8::rt;
namespace mips = dq8::test::mips;
using Catch::Matchers::ContainsSubstring;
using dq8::test::IrxBuilder;

namespace {

std::uint32_t word(const std::vector<std::uint8_t>& image, std::uint32_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, image.data() + at, 4);
    return v;
}

// A module with every relocation kind: a jal, an la into the data, a data pointer into the text.
struct Sample {
    Sample() : b("SAMPLE", 0x0203) {
        table = b.words({1, 2, 3});
        b.imports("thbase", {6, 20});
        b.imports("sysclib", {12});
        start = b.here();
        b.jal(b.stub("thbase", 20));
        la_at = b.text_size();
        b.la(mips::t0, table);
        b.ret();
        helper = b.here();
        b.ret();
        b.pointer(table, 4, helper);
        b.bss(0x100);
        b.entry(start);
    }
    IrxBuilder b;
    IrxBuilder::Label start;
    IrxBuilder::Label helper;
    IrxBuilder::Label table;
    std::uint32_t la_at = 0;
};

} // namespace

TEST_CASE("irx loader: parse reads the .iopmod record, the image and the relocations", "[iop][irx]") {
    Sample s;
    const rt::IrxModule m = rt::parse_irx(s.b.build());
    CHECK(m.name == "SAMPLE");
    CHECK(m.version == 0x0203);
    CHECK(m.entry == s.b.offset_of(s.start));
    CHECK(m.text_size == s.b.text_size());
    CHECK(m.bss_size == 0x100);
    CHECK(m.mem_size == m.bytes.size() + 0x100);
    CHECK(m.relocs.size() == 4); // jal, hi16, lo16, data pointer
}

TEST_CASE("irx loader: relocation moves jumps, address pairs and data pointers to the base", "[iop][irx]") {
    Sample s;
    const rt::IrxModule m = rt::parse_irx(s.b.build());
    constexpr std::uint32_t base = 0x00034500;
    const std::vector<std::uint8_t> image = rt::relocate_irx(m, base);
    REQUIRE(image.size() == m.mem_size);
    const std::uint32_t stub = base + s.b.offset_of(s.b.stub("thbase", 20));
    const std::uint32_t entry = s.b.offset_of(s.start);
    CHECK(word(image, entry) == mips::jal(stub));
    const std::uint32_t table = base + s.b.offset_of(s.table);
    const std::uint32_t hi = word(image, s.la_at) & 0xFFFFu;
    const std::uint32_t lo = word(image, s.la_at + 4) & 0xFFFFu;
    CHECK((hi << 16) + static_cast<std::uint32_t>(static_cast<std::int16_t>(lo)) == table);
    CHECK(word(image, s.b.offset_of(s.table) + 4) == base + s.b.offset_of(s.helper));
    CHECK(word(image, s.b.offset_of(s.table) + 8) == 3);
    for (std::uint32_t i = m.mem_size - 0x100; i < m.mem_size; ++i) {
        CHECK(image[i] == 0);
    }
}

TEST_CASE("irx loader: import tables are found in the text with their stubs", "[iop][irx]") {
    Sample s;
    const rt::IrxModule m = rt::parse_irx(s.b.build());
    rt::IopMemory ram;
    constexpr std::uint32_t base = 0x00040000;
    ram.write(base, rt::relocate_irx(m, base));
    const auto tables = rt::find_irx_imports(ram, base, m.text_size);
    REQUIRE(tables.size() == 2);
    CHECK(tables[0].library == "thbase");
    CHECK(tables[0].version == 0x0101);
    REQUIRE(tables[0].stubs.size() == 2);
    CHECK(tables[0].stubs[0].ordinal == 6);
    CHECK(tables[0].stubs[1].ordinal == 20);
    CHECK(tables[0].stubs[1].address == base + s.b.offset_of(s.b.stub("thbase", 20)));
    CHECK(tables[1].library == "sysclib");
    CHECK(tables[1].stubs.at(0).ordinal == 12);
}

TEST_CASE("irx loader: a constant equal to the import magic is not a table", "[iop][irx]") {
    IrxBuilder b("MAGIC");
    const auto start = b.here();
    b.emit(rt::irx_import_magic);
    b.emit(0);
    b.emit(0x0101);
    b.emit(0);
    b.emit(0);
    b.emit(0);
    b.emit(0);
    b.ret();
    b.entry(start);
    const rt::IrxModule m = rt::parse_irx(b.build());
    rt::IopMemory ram;
    ram.write(0x40000, rt::relocate_irx(m, 0x40000));
    CHECK(rt::find_irx_imports(ram, 0x40000, m.text_size).empty());
}

TEST_CASE("irx loader: refuses what it cannot load faithfully", "[iop][irx]") {
    SECTION("another ELF type") {
        Sample s;
        s.b.set_elf_type(2);
        CHECK_THROWS_WITH(rt::parse_irx(s.b.build()), ContainsSubstring("0xFF80"));
    }
    SECTION("an unknown relocation type") {
        Sample s;
        s.b.raw_relocation(0, 7);
        CHECK_THROWS_WITH(rt::parse_irx(s.b.build()), ContainsSubstring("relocation type 7"));
    }
    SECTION("a HI16 with no LO16") {
        Sample s;
        s.b.raw_relocation(0, 5);
        const rt::IrxModule m = rt::parse_irx(s.b.build());
        CHECK_THROWS_WITH(rt::relocate_irx(m, 0x10000), ContainsSubstring("no LO16"));
    }
    SECTION("not an ELF") {
        std::vector<std::uint8_t> junk(100, 0x41);
        CHECK_THROWS_WITH(rt::parse_irx(junk), ContainsSubstring("not an ELF"));
    }
}

TEST_CASE("iop libraries: HLE first, then guest exports by name and major version", "[iop][irx]") {
    rt::IopLibraries libs;
    libs.add_hle("thbase", {{4, 10}, {6, 11}});
    rt::IopMemory ram;
    IrxBuilder b("EXPORTER");
    const auto start = b.here();
    b.ret();
    const auto f4 = b.here();
    b.ret();
    const auto table = b.exports("mylib", {start, start, start, start, f4}, 0x0102);
    b.entry(start);
    const rt::IrxModule m = rt::parse_irx(b.build());
    constexpr std::uint32_t base = 0x00050000;
    ram.write(base, rt::relocate_irx(m, base));
    const std::uint32_t at = base + b.offset_of(table);

    CHECK(libs.register_guest(ram, at, "EXPORTER") == 0);
    CHECK(libs.register_guest(ram, at, "EXPORTER") == rt::iop_error::library_found);

    auto t = libs.resolve("thbase", 0x0101, 6);
    CHECK(t.kind == rt::IopLinkTarget::Kind::hle);
    CHECK(t.slot == 11);
    CHECK(libs.resolve("thbase", 0x0101, 25).kind == rt::IopLinkTarget::Kind::unimplemented);
    t = libs.resolve("mylib", 0x0101, 4);
    CHECK(t.kind == rt::IopLinkTarget::Kind::guest);
    CHECK(t.address == base + b.offset_of(f4));
    CHECK(libs.resolve("mylib", 0x0101, 5).kind == rt::IopLinkTarget::Kind::unimplemented);
    CHECK(libs.resolve("mylib", 0x0201, 4).kind == rt::IopLinkTarget::Kind::missing_library);
    CHECK(libs.resolve("nosuch", 0x0101, 4).kind == rt::IopLinkTarget::Kind::missing_library);

    CHECK(libs.release_guest(at) == 0);
    CHECK(libs.release_guest(at) == rt::iop_error::library_notfound);
    CHECK(libs.resolve("mylib", 0x0101, 4).kind == rt::IopLinkTarget::Kind::missing_library);

    // A guest library may not take an HLE library's name, and the table must carry the export magic.
    IrxBuilder evil("EVIL");
    const auto e0 = evil.here();
    evil.ret();
    const auto evil_table = evil.exports("thbase", {e0});
    evil.entry(e0);
    const rt::IrxModule em = rt::parse_irx(evil.build());
    ram.write(0x60000, rt::relocate_irx(em, 0x60000));
    CHECK(libs.register_guest(ram, 0x60000 + evil.offset_of(evil_table), "EVIL") ==
          rt::iop_error::library_found);
    CHECK(libs.register_guest(ram, 0x60000, "EVIL") == rt::iop_error::illegal_library);
}
