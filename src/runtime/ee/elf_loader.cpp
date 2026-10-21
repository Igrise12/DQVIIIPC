// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/elf_loader.h"

#include "dq8/core/elf_headers.h"
#include "dq8/core/error.h"
#include "runtime/ee/guest_stop.h"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace dq8::rt {

using core::Error;
using core::ErrorCode;

LoadedElf load_elf(std::span<const std::uint8_t> file, GuestMemory& memory) {
    const core::ElfHeaders headers = core::parse_elf_headers(file);
    if (headers.machine != core::elf_em_mips) {
        throw Error(ErrorCode::elf_format,
                    "ELF machine is " + std::to_string(headers.machine) + ", expected EM_MIPS (8)");
    }
    if (headers.type != elf_type_exec) {
        throw Error(ErrorCode::elf_format,
                    "ELF type is " + std::to_string(headers.type) + ", expected ET_EXEC (2)");
    }
    LoadedElf out;
    out.entry = headers.entry;
    std::size_t index = 0;
    for (const core::ElfSegment& s : headers.segments) {
        const std::size_t i = index++;
        if (s.type != core::elf_pt_load || s.memsz == 0) {
            continue;
        }
        const std::string where = "segment " + std::to_string(i) + " (vaddr " + hex32(s.vaddr) + ")";
        if (std::uint64_t{s.offset} + s.filesz > file.size()) {
            throw Error(ErrorCode::elf_format, where + ": file bytes lie beyond the end of the file");
        }
        try {
            // Zero the whole range first: that also checks all of it before anything is copied.
            memory.fill(s.vaddr, s.memsz, 0);
            memory.write_bytes(s.vaddr, file.subspan(s.offset, s.filesz));
        } catch (const std::out_of_range&) {
            throw Error(ErrorCode::elf_format, where + ", memsz " + hex32(s.memsz) +
                                                   ": does not fit in guest RAM or the scratchpad");
        }
        out.segments.push_back({s.vaddr, s.filesz, s.memsz, s.flags});
    }
    if (out.segments.empty()) {
        throw Error(ErrorCode::elf_format, "ELF file has no loadable segment");
    }
    return out;
}

} // namespace dq8::rt
