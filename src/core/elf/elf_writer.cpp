// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/elf_writer.h"

#include "dq8/core/elf_headers.h"
#include "dq8/core/error.h"
#include "dq8/core/hash.h"
#include "platform/files.h"

#include <algorithm>
#include <set>

namespace dq8::core {

namespace {

constexpr std::uint64_t max_file_size = 0xFFFFFFFFu;

void put16(std::vector<std::uint8_t>& out, std::size_t at, std::uint16_t v) {
    out[at] = static_cast<std::uint8_t>(v);
    out[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

void put32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

std::uint64_t align_up(std::uint64_t v, std::uint64_t align) {
    return (v + align - 1) / align * align;
}

std::string hex(std::uint32_t v) {
    return "0x" + to_hex(v);
}

[[noreturn]] void layout_error(const std::string& message) {
    throw Error(ErrorCode::elf_layout, message);
}

void validate(const std::vector<ElfSection>& sections) {
    std::set<std::string> names;
    for (const ElfSection& s : sections) {
        const std::string label = "section '" + s.name + "'";
        if (s.name.empty() || s.name.find('\0') != std::string::npos) {
            layout_error("a section has an empty name or one containing NUL");
        }
        if (s.name == ".shstrtab" || !names.insert(s.name).second) {
            layout_error(label + " is reserved or appears twice");
        }
        if (s.type != elf_sht_progbits && s.type != elf_sht_nobits) {
            layout_error(label + " has type " + std::to_string(s.type) +
                         "; only PROGBITS and NOBITS are written");
        }
        if (s.align == 0 || (s.align & (s.align - 1)) != 0) {
            layout_error(label + " has alignment " + std::to_string(s.align) + ", not a power of two");
        }
        if (s.address % s.align != 0) {
            layout_error(label + " at " + hex(s.address) + " is not aligned to " + std::to_string(s.align));
        }
        if (s.type == elf_sht_nobits && !s.data.empty()) {
            layout_error(label + " is NOBITS but carries data");
        }
        if (s.type == elf_sht_progbits && s.nobits_size != 0) {
            layout_error(label + " is PROGBITS but sets nobits_size");
        }
        if (s.data.size() > max_file_size ||
            static_cast<std::uint64_t>(s.address) + s.size() > std::uint64_t{1} << 32) {
            layout_error(label + " at " + hex(s.address) + " runs past the end of the 32-bit address space");
        }
    }
    // Allocated sections must not overlap in memory: the loader (ours, and the recompiler's view) would
    // otherwise see two contents for one address.
    std::vector<const ElfSection*> alloc;
    for (const ElfSection& s : sections) {
        if ((s.flags & elf_shf_alloc) != 0 && s.size() != 0) {
            alloc.push_back(&s);
        }
    }
    std::sort(alloc.begin(), alloc.end(),
              [](const ElfSection* a, const ElfSection* b) { return a->address < b->address; });
    for (std::size_t i = 1; i < alloc.size(); ++i) {
        const ElfSection& prev = *alloc[i - 1];
        if (static_cast<std::uint64_t>(prev.address) + prev.size() > alloc[i]->address) {
            layout_error("sections '" + prev.name + "' and '" + alloc[i]->name + "' overlap at " +
                         hex(alloc[i]->address));
        }
    }
}

} // namespace

ElfWriter& ElfWriter::add_section(ElfSection section) {
    sections_.push_back(std::move(section));
    return *this;
}

std::vector<std::uint8_t> ElfWriter::build() const {
    validate(sections_);

    // Program headers: one PT_LOAD per allocated, non-empty section, sorted by address (the ELF rule).
    std::vector<std::size_t> loads;
    for (std::size_t i = 0; i < sections_.size(); ++i) {
        if ((sections_[i].flags & elf_shf_alloc) != 0 && sections_[i].size() != 0) {
            loads.push_back(i);
        }
    }
    std::sort(loads.begin(), loads.end(),
              [&](std::size_t a, std::size_t b) { return sections_[a].address < sections_[b].address; });

    // File layout: header, program headers, section contents (offset congruent to the address modulo the
    // alignment), the name table, then the section header table.
    std::uint64_t offset = elf_header_size + loads.size() * elf_program_header_size;
    std::vector<std::uint64_t> section_offsets(sections_.size());
    for (std::size_t i = 0; i < sections_.size(); ++i) {
        offset = align_up(offset, sections_[i].align);
        section_offsets[i] = offset;
        offset += sections_[i].data.size();
    }
    std::string shstrtab(1, '\0');
    std::vector<std::uint32_t> name_offsets;
    for (const ElfSection& s : sections_) {
        name_offsets.push_back(static_cast<std::uint32_t>(shstrtab.size()));
        shstrtab += s.name;
        shstrtab.push_back('\0');
    }
    const auto shstrtab_name = static_cast<std::uint32_t>(shstrtab.size());
    shstrtab += ".shstrtab";
    shstrtab.push_back('\0');
    const std::uint64_t shstrtab_offset = offset;
    offset += shstrtab.size();
    const std::uint64_t shoff = align_up(offset, 4);
    const std::size_t shnum = sections_.size() + 2; // null section, ours, .shstrtab
    const std::uint64_t total = shoff + shnum * elf_section_header_size;
    if (total > max_file_size) {
        layout_error("the ELF would be " + std::to_string(total) + " bytes, more than 4 GiB");
    }

    std::vector<std::uint8_t> out(static_cast<std::size_t>(total), 0);
    // e_ident: magic, ELFCLASS32, ELFDATA2LSB, EV_CURRENT, System V ABI.
    out[0] = 0x7F;
    out[1] = 'E';
    out[2] = 'L';
    out[3] = 'F';
    out[4] = 1;
    out[5] = 1;
    out[6] = 1;
    put16(out, 16, elf_et_exec);
    put16(out, 18, machine_);
    put32(out, 20, 1); // e_version
    put32(out, 24, entry_);
    put32(out, 28, loads.empty() ? 0 : static_cast<std::uint32_t>(elf_header_size));
    put32(out, 32, static_cast<std::uint32_t>(shoff));
    put32(out, 36, flags_);
    put16(out, 40, static_cast<std::uint16_t>(elf_header_size));
    put16(out, 42, static_cast<std::uint16_t>(elf_program_header_size));
    put16(out, 44, static_cast<std::uint16_t>(loads.size()));
    put16(out, 46, static_cast<std::uint16_t>(elf_section_header_size));
    put16(out, 48, static_cast<std::uint16_t>(shnum));
    put16(out, 50, static_cast<std::uint16_t>(shnum - 1)); // e_shstrndx: .shstrtab is last

    for (std::size_t n = 0; n < loads.size(); ++n) {
        const ElfSection& s = sections_[loads[n]];
        const std::size_t at = elf_header_size + n * elf_program_header_size;
        std::uint32_t pflags = elf_pf_r;
        pflags |= (s.flags & elf_shf_write) != 0 ? elf_pf_w : 0;
        pflags |= (s.flags & elf_shf_execinstr) != 0 ? elf_pf_x : 0;
        put32(out, at, elf_pt_load);
        put32(out, at + 4, static_cast<std::uint32_t>(section_offsets[loads[n]]));
        put32(out, at + 8, s.address);  // p_vaddr
        put32(out, at + 12, s.address); // p_paddr
        put32(out, at + 16, static_cast<std::uint32_t>(s.data.size()));
        put32(out, at + 20, s.size());
        put32(out, at + 24, pflags);
        put32(out, at + 28, s.align);
    }

    for (std::size_t i = 0; i < sections_.size(); ++i) {
        std::copy(sections_[i].data.begin(), sections_[i].data.end(),
                  out.begin() + static_cast<std::ptrdiff_t>(section_offsets[i]));
    }
    std::copy(shstrtab.begin(), shstrtab.end(), out.begin() + static_cast<std::ptrdiff_t>(shstrtab_offset));

    auto section_header = [&](std::size_t index, std::uint32_t name, std::uint32_t type, std::uint32_t flags,
                              std::uint32_t addr, std::uint64_t off, std::uint32_t size,
                              std::uint32_t align) {
        const std::size_t at = static_cast<std::size_t>(shoff) + index * elf_section_header_size;
        put32(out, at, name);
        put32(out, at + 4, type);
        put32(out, at + 8, flags);
        put32(out, at + 12, addr);
        put32(out, at + 16, static_cast<std::uint32_t>(off));
        put32(out, at + 20, size);
        put32(out, at + 32, align); // sh_link, sh_info stay 0; sh_entsize stays 0
    };
    for (std::size_t i = 0; i < sections_.size(); ++i) {
        const ElfSection& s = sections_[i];
        section_header(i + 1, name_offsets[i], s.type, s.flags, s.address, section_offsets[i], s.size(),
                       s.align);
    }
    section_header(shnum - 1, shstrtab_name, elf_sht_strtab, 0, 0, shstrtab_offset,
                   static_cast<std::uint32_t>(shstrtab.size()), 1);
    return out;
}

void ElfWriter::write(const std::filesystem::path& file) const {
    const std::vector<std::uint8_t> bytes = build();
    platform::write_file_atomic(file, bytes);
}

} // namespace dq8::core
