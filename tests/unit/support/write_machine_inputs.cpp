// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Writes the inputs of the synthetic machine run (machine_unit.h): the ELF (a zero-filled segment over the
// unit's table range, where native code runs, and the data segment) and a synthetic ISO holding the file the
// program reads. Usage: dq8_machine_inputs <out.elf> <out.iso>
#include "support/elf_image.h"
#include "support/iso_builder.h"
#include "support/machine_unit.h"

#include <cstdio>
#include <exception>
#include <fstream>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fputs("usage: dq8_machine_inputs <out.elf> <out.iso>\n", stderr);
        return 2;
    }
    try {
        using namespace machine_unit;
        const std::uint32_t code = table_end - table_base;
        const std::vector<std::uint8_t> elf =
            test_elf::build(entry, {test_elf::Segment{table_base, std::vector<std::uint8_t>(code, 0), code},
                                    test_elf::Segment{data_base, data_segment(), data_end - data_base}});
        std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(elf.data()), static_cast<std::streamsize>(elf.size()));
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", argv[1]);
            return 2;
        }
        dq8::test::IsoBuilder iso;
        iso.add_file(disc_file, disc_file_bytes());
        iso.write(argv[2]);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dq8_machine_inputs: %s\n", e.what());
        return 2;
    }
}
