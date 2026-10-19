// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/elf_headers.h"

#include "dq8/core/error.h"

#include <string>

namespace dq8::core {

namespace {

constexpr std::size_t phdr_size = 32;

std::uint16_t le16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t le32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

} // namespace

std::vector<const ElfSegment*> ElfHeaders::load_segments() const {
    std::vector<const ElfSegment*> out;
    for (const ElfSegment& s : segments) {
        if (s.type == elf_pt_load) {
            out.push_back(&s);
        }
    }
    return out;
}

std::size_t elf_program_table_end(std::span<const std::uint8_t> h) {
    if (h.size() < elf_header_size) {
        throw Error(ErrorCode::elf_format,
                    "file is shorter than an ELF header (" + std::to_string(h.size()) + " bytes)");
    }
    if (!(h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F')) {
        throw Error(ErrorCode::elf_format, "not an ELF file (bad magic)");
    }
    if (h[4] != 1) {
        throw Error(ErrorCode::elf_format, "not a 32-bit ELF file (EI_CLASS " + std::to_string(h[4]) + ")");
    }
    if (h[5] != 1) {
        throw Error(ErrorCode::elf_format,
                    "not a little-endian ELF file (EI_DATA " + std::to_string(h[5]) + ")");
    }
    if (h[6] != 1) {
        throw Error(ErrorCode::elf_format, "unsupported ELF version " + std::to_string(h[6]));
    }
    const std::uint32_t phoff = le32(&h[28]);
    const std::uint16_t phentsize = le16(&h[42]);
    const std::uint16_t phnum = le16(&h[44]);
    if (phnum == 0) {
        throw Error(ErrorCode::elf_format, "ELF file has no program headers");
    }
    if (phentsize < phdr_size) {
        throw Error(ErrorCode::elf_format,
                    "program header entry size " + std::to_string(phentsize) + " is too small");
    }
    if (phoff < elf_header_size) {
        throw Error(ErrorCode::elf_format,
                    "program header table offset " + std::to_string(phoff) + " overlaps the ELF header");
    }
    const std::uint64_t end = static_cast<std::uint64_t>(phoff) +
                              static_cast<std::uint64_t>(phentsize) * static_cast<std::uint64_t>(phnum);
    if (end > 0xFFFFFFFFull) {
        throw Error(ErrorCode::elf_format, "program header table lies beyond 4 GiB");
    }
    return static_cast<std::size_t>(end);
}

ElfHeaders parse_elf_headers(std::span<const std::uint8_t> data) {
    const std::size_t needed = elf_program_table_end(data);
    if (data.size() < needed) {
        throw Error(ErrorCode::elf_format, "program header table needs " + std::to_string(needed) +
                                               " bytes but only " + std::to_string(data.size()) +
                                               " are available");
    }
    ElfHeaders h;
    h.type = le16(&data[16]);
    h.machine = le16(&data[18]);
    h.entry = le32(&data[24]);
    h.phoff = le32(&data[28]);
    h.shoff = le32(&data[32]);
    h.flags = le32(&data[36]);
    h.phentsize = le16(&data[42]);
    h.phnum = le16(&data[44]);
    h.shentsize = le16(&data[46]);
    h.shnum = le16(&data[48]);
    h.segments.reserve(h.phnum);
    for (std::uint16_t i = 0; i < h.phnum; ++i) {
        const std::uint8_t* p = &data[h.phoff + static_cast<std::size_t>(i) * h.phentsize];
        ElfSegment s;
        s.type = le32(p);
        s.offset = le32(p + 4);
        s.vaddr = le32(p + 8);
        s.paddr = le32(p + 12);
        s.filesz = le32(p + 16);
        s.memsz = le32(p + 20);
        s.flags = le32(p + 24);
        s.align = le32(p + 28);
        if (s.filesz > s.memsz && s.type == elf_pt_load) {
            throw Error(ErrorCode::elf_format,
                        "program header " + std::to_string(i) + " has filesz larger than memsz");
        }
        h.segments.push_back(s);
    }
    return h;
}

} // namespace dq8::core
