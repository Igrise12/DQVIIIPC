// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Writes ELF32 little-endian executables (ET_EXEC): the header, one PT_LOAD per allocated section in address
// order, the section contents, a section-name string table and the section header table. Enough to hand the
// recompiler a code unit (overlay_elf.h) or a hand-assembled test program; no symbols, relocations or dynamic
// sections. The reader side is elf_headers.h.
namespace dq8::core {

inline constexpr std::uint16_t elf_et_exec = 2;
inline constexpr std::uint32_t elf_sht_progbits = 1;
inline constexpr std::uint32_t elf_sht_strtab = 3;
inline constexpr std::uint32_t elf_sht_nobits = 8;
inline constexpr std::uint32_t elf_shf_write = 0x1;
inline constexpr std::uint32_t elf_shf_alloc = 0x2;
inline constexpr std::uint32_t elf_shf_execinstr = 0x4;
inline constexpr std::size_t elf_program_header_size = 32;
inline constexpr std::size_t elf_section_header_size = 40;

struct ElfSection {
    std::string name;                      // ".text", ".bss", ...: unique, non-empty, not ".shstrtab"
    std::uint32_t type = elf_sht_progbits; // elf_sht_progbits or elf_sht_nobits
    std::uint32_t flags = elf_shf_alloc;   // elf_shf_* bits
    std::uint32_t address = 0;             // a multiple of `align`
    std::uint32_t align = 4;               // a power of two
    std::vector<std::uint8_t> data;        // PROGBITS contents; their size is the section's size
    std::uint32_t nobits_size = 0;         // NOBITS size; `data` must then be empty

    std::uint32_t size() const noexcept {
        return type == elf_sht_nobits ? nobits_size : static_cast<std::uint32_t>(data.size());
    }
};

class ElfWriter {
  public:
    ElfWriter& set_entry(std::uint32_t address) noexcept {
        entry_ = address;
        return *this;
    }
    ElfWriter& set_flags(std::uint32_t e_flags) noexcept {
        flags_ = e_flags;
        return *this;
    }
    ElfWriter& set_machine(std::uint16_t machine) noexcept {
        machine_ = machine;
        return *this;
    }
    ElfWriter& add_section(ElfSection section);

    // Lays the file out and serialises it. Every allocated section with a non-zero size gets a PT_LOAD
    // (flags R, plus W for SHF_WRITE and X for SHF_EXECINSTR; NOBITS sections load with filesz 0) whose file
    // offset is congruent to its address modulo its alignment. Throws Error(elf_layout) for an empty or
    // duplicate name, a bad type or alignment, a misaligned address, PROGBITS with nobits_size, NOBITS with
    // data, a section that wraps past 4 GiB, or two allocated sections that overlap in memory.
    std::vector<std::uint8_t> build() const;

    // build(), then replaces `file` through a temporary file in the same directory. Throws io_write.
    void write(const std::filesystem::path& file) const;

  private:
    std::uint32_t entry_ = 0;
    std::uint32_t flags_ = 0;
    std::uint16_t machine_ = 8; // EM_MIPS
    std::vector<ElfSection> sections_;
};

} // namespace dq8::core
