// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/elf_headers.h"
#include "dq8/core/error.h"
#include "support/iso_builder.h"

#include <catch2/catch_test_macros.hpp>

using dq8::core::Error;
using dq8::core::ErrorCode;

TEST_CASE("ELF32 header and program headers are parsed", "[core][elf]") {
    const auto elf =
        dq8::test::fake_boot_elf(0x00100008, 0x00100000, 0x1000, 0x00200000, {0x300, 0x40}, 600, 9);
    CHECK(dq8::core::elf_program_table_end(elf) == 52 + 3 * 32);
    const auto h = dq8::core::parse_elf_headers(elf);
    CHECK(h.machine == dq8::core::elf_em_mips);
    CHECK(h.type == 2);
    CHECK(h.entry == 0x00100008);
    CHECK(h.phnum == 3);
    REQUIRE(h.segments.size() == 3);
    const auto loads = h.load_segments();
    REQUIRE(loads.size() == 3);
    CHECK(loads[0]->vaddr == 0x00100000);
    CHECK(loads[0]->filesz == 600);
    CHECK(loads[0]->memsz == 0x1000);
    CHECK(loads[1]->vaddr == 0x00200000);
    CHECK(loads[1]->filesz == 0);
    CHECK(loads[1]->memsz == 0x300);
    CHECK(loads[2]->memsz == 0x40);
    CHECK((loads[0]->flags & dq8::core::elf_pf_x) != 0);
}

TEST_CASE("malformed ELF headers are refused", "[core][elf]") {
    auto elf = dq8::test::fake_boot_elf(0x100008, 0x100000, 0x1000, 0x200000, {0x300}, 64, 1);
    auto expect = [](const std::vector<std::uint8_t>& data) {
        try {
            (void)dq8::core::parse_elf_headers(data);
        } catch (const Error& e) {
            CHECK(e.code() == ErrorCode::elf_format);
            return;
        }
        FAIL("expected elf_format");
    };
    // at() rather than [] so GCC's -Wnull-dereference sees a checked path.
    std::vector<std::uint8_t> bad = elf;
    bad.at(1) = 'X';
    expect(bad);
    bad = elf;
    bad.at(4) = 2; // ELFCLASS64
    expect(bad);
    bad = elf;
    bad.at(5) = 2; // big-endian
    expect(bad);
    bad = elf;
    bad.resize(40); // shorter than a header
    expect(bad);
    bad = elf;
    bad.resize(60); // header present, program table cut off
    expect(bad);
    bad = elf;
    bad.at(44) = 0; // phnum 0
    bad.at(45) = 0;
    expect(bad);
}
