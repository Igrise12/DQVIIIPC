// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// A minimal ELF32 little-endian header and program-header parser: enough to read a PS2 boot executable's
// entry point and load segments for verification. Section headers, symbols and relocations are not parsed.
namespace dq8::core {

inline constexpr std::size_t elf_header_size = 52;
inline constexpr std::uint32_t elf_pt_load = 1;
inline constexpr std::uint16_t elf_em_mips = 8;
inline constexpr std::uint32_t elf_pf_x = 1;
inline constexpr std::uint32_t elf_pf_w = 2;
inline constexpr std::uint32_t elf_pf_r = 4;

struct ElfSegment {
    std::uint32_t type = 0;
    std::uint32_t offset = 0;
    std::uint32_t vaddr = 0;
    std::uint32_t paddr = 0;
    std::uint32_t filesz = 0;
    std::uint32_t memsz = 0;
    std::uint32_t flags = 0;
    std::uint32_t align = 0;
};

struct ElfHeaders {
    std::uint16_t type = 0;
    std::uint16_t machine = 0;
    std::uint32_t entry = 0;
    std::uint32_t phoff = 0;
    std::uint32_t shoff = 0;
    std::uint32_t flags = 0;
    std::uint16_t phentsize = 0;
    std::uint16_t phnum = 0;
    std::uint16_t shentsize = 0;
    std::uint16_t shnum = 0;
    std::vector<ElfSegment> segments; // every program header, in table order

    std::vector<const ElfSegment*> load_segments() const;
};

// Validates the identification bytes and fixed header (the first elf_header_size bytes) and returns how many
// bytes from the start of the file the program header table needs: phoff + phnum * phentsize.
// Throws Error(ErrorCode::elf_format).
std::size_t elf_program_table_end(std::span<const std::uint8_t> header);

// Parses the header and the program header table. `data` must start at file offset 0 and cover
// elf_program_table_end(data). Throws Error(ErrorCode::elf_format).
ElfHeaders parse_elf_headers(std::span<const std::uint8_t> data);

} // namespace dq8::core
