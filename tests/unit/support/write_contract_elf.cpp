// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Writes the ELF that goes with the contract unit (contract_unit.h): one zero-filled load segment covering
// the unit's table range, entry at the unit's entry. Native code runs, so the segment's bytes do not matter.
// Usage: dq8_contract_elf <out.elf>
#include "support/contract_unit.h"
#include "support/elf_image.h"

#include <cstdio>
#include <fstream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fputs("usage: dq8_contract_elf <out.elf>\n", stderr);
        return 2;
    }
    const std::uint32_t size = contract::table_end - contract::table_base;
    const std::vector<std::uint8_t> elf = test_elf::build(
        contract::entry,
        {test_elf::Segment{contract::table_base, std::vector<std::uint8_t>(size, 0), size * 2}});
    std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(elf.data()), static_cast<std::streamsize>(elf.size()));
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        return 2;
    }
    return 0;
}
