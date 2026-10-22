// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/loader/irx_image.h"

#include "runtime/ee/guest_stop.h"
#include "runtime/iop/iop_memory.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace dq8::rt {

namespace {

constexpr std::uint16_t em_mips = 8;
constexpr std::uint32_t pt_load = 1;
constexpr std::uint32_t sht_rela = 4;
constexpr std::uint32_t sht_rel = 9;
constexpr std::uint8_t r_mips_none = 0;
constexpr std::uint8_t r_mips_32 = 2;
constexpr std::uint8_t r_mips_26 = 4;
constexpr std::uint8_t r_mips_hi16 = 5;
constexpr std::uint8_t r_mips_lo16 = 6;
// The image size the loader accepts (a sound module is tens of KiB).
constexpr std::uint32_t max_image = 1u << 20;

std::uint16_t get16(std::span<const std::uint8_t> b, std::size_t off) {
    if (off + 2 > b.size()) {
        throw IrxError("the file ends inside its headers");
    }
    return static_cast<std::uint16_t>(b[off] | (b[off + 1] << 8));
}

std::uint32_t get32(std::span<const std::uint8_t> b, std::size_t off) {
    if (off + 4 > b.size()) {
        throw IrxError("the file ends inside its headers");
    }
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

std::uint32_t word_at(const std::vector<std::uint8_t>& image, std::uint32_t offset) {
    std::uint32_t v = 0;
    std::memcpy(&v, image.data() + offset, 4);
    return v;
}

void put_word(std::vector<std::uint8_t>& image, std::uint32_t offset, std::uint32_t v) {
    std::memcpy(image.data() + offset, &v, 4);
}

std::string hex(std::uint32_t v) {
    return hex32(v);
}

} // namespace

IrxModule parse_irx(std::span<const std::uint8_t> f) {
    if (f.size() < 52 || f[0] != 0x7F || f[1] != 'E' || f[2] != 'L' || f[3] != 'F') {
        throw IrxError("not an ELF file");
    }
    if (f[4] != 1 || f[5] != 1) {
        throw IrxError("not a 32-bit little-endian ELF");
    }
    const std::uint16_t type = get16(f, 16);
    if (type != irx_elf_type) {
        throw IrxError("ELF type " + hex(type) + " is not an IOP relocatable module (0xFF80)");
    }
    if (get16(f, 18) != em_mips) {
        throw IrxError("not a MIPS ELF");
    }
    const std::uint32_t phoff = get32(f, 28);
    const std::uint32_t shoff = get32(f, 32);
    const std::uint16_t phentsize = get16(f, 42);
    const std::uint16_t phnum = get16(f, 44);
    const std::uint16_t shentsize = get16(f, 46);
    const std::uint16_t shnum = get16(f, 48);
    if (phnum == 0 || phentsize < 32) {
        throw IrxError("no program headers");
    }

    IrxModule m;
    bool have_iopmod = false;
    bool have_load = false;
    for (std::uint16_t i = 0; i < phnum; ++i) {
        const std::size_t ph = static_cast<std::size_t>(phoff) + static_cast<std::size_t>(i) * phentsize;
        const std::uint32_t p_type = get32(f, ph);
        const std::uint32_t p_offset = get32(f, ph + 4);
        const std::uint32_t p_vaddr = get32(f, ph + 8);
        const std::uint32_t p_filesz = get32(f, ph + 16);
        const std::uint32_t p_memsz = get32(f, ph + 20);
        if (p_type == irx_iopmod_type) {
            if (p_filesz < 27 || p_offset > f.size() || p_filesz > f.size() - p_offset) {
                throw IrxError("the .iopmod record is truncated");
            }
            const auto rec = f.subspan(p_offset, p_filesz);
            m.entry = get32(rec, 4);
            m.gp = get32(rec, 8);
            m.text_size = get32(rec, 12);
            m.data_size = get32(rec, 16);
            m.bss_size = get32(rec, 20);
            m.version = get16(rec, 24);
            for (std::size_t k = 26; k < rec.size() && rec[k] != 0; ++k) {
                m.name.push_back(static_cast<char>(rec[k]));
            }
            have_iopmod = true;
        } else if (p_type == pt_load) {
            if (have_load) {
                throw IrxError("more than one PT_LOAD segment");
            }
            if (p_vaddr != 0) {
                throw IrxError("the PT_LOAD segment is linked at " + hex(p_vaddr) + ", not 0");
            }
            if (p_memsz < p_filesz || p_memsz > max_image || p_offset > f.size() ||
                p_filesz > f.size() - p_offset) {
                throw IrxError("the PT_LOAD segment does not fit the file");
            }
            m.bytes.assign(f.begin() + p_offset, f.begin() + p_offset + p_filesz);
            m.mem_size = p_memsz;
            have_load = true;
        }
    }
    if (!have_iopmod) {
        throw IrxError("no .iopmod program header (type 0x70000080)");
    }
    if (!have_load) {
        throw IrxError("no PT_LOAD segment");
    }
    if (m.text_size > m.mem_size || m.entry >= m.mem_size || (m.entry & 3u) != 0) {
        throw IrxError("the .iopmod entry " + hex(m.entry) + " or text size " + hex(m.text_size) +
                       " lies outside the image");
    }

    if (shnum != 0 && shentsize < 40) {
        throw IrxError("section headers are too small");
    }
    for (std::uint16_t i = 0; i < shnum; ++i) {
        const std::size_t sh = static_cast<std::size_t>(shoff) + static_cast<std::size_t>(i) * shentsize;
        const std::uint32_t sh_type = get32(f, sh + 4);
        if (sh_type == sht_rela) {
            throw IrxError("section " + std::to_string(i) + " holds RELA relocations");
        }
        if (sh_type != sht_rel) {
            continue;
        }
        const std::uint32_t off = get32(f, sh + 16);
        const std::uint32_t size = get32(f, sh + 20);
        const std::uint32_t entsize = get32(f, sh + 36);
        if (entsize != 8 || off > f.size() || size > f.size() - off || size % 8 != 0) {
            throw IrxError("relocation section " + std::to_string(i) + " is malformed");
        }
        for (std::uint32_t e = 0; e < size; e += 8) {
            const std::uint32_t r_offset = get32(f, off + e);
            const std::uint32_t r_info = get32(f, off + e + 4);
            const auto r_type = static_cast<std::uint8_t>(r_info & 0xFFu);
            const std::uint32_t symbol = r_info >> 8;
            if (r_type == r_mips_none) {
                continue;
            }
            if (r_type != r_mips_32 && r_type != r_mips_26 && r_type != r_mips_hi16 &&
                r_type != r_mips_lo16) {
                throw IrxError("relocation type " + std::to_string(r_type) + " at image offset " +
                               hex(r_offset) + " is not supported");
            }
            if (symbol != 0) {
                throw IrxError("relocation at image offset " + hex(r_offset) + " names symbol " +
                               std::to_string(symbol) + "; only base-relative relocations are supported");
            }
            if ((r_offset & 3u) != 0 || r_offset + 4 > m.bytes.size()) {
                throw IrxError("relocation at image offset " + hex(r_offset) +
                               " lies outside the loaded data");
            }
            m.relocs.push_back(IrxRelocation{r_offset, r_type});
        }
    }
    return m;
}

std::vector<std::uint8_t> relocate_irx(const IrxModule& m, std::uint32_t base) {
    std::vector<std::uint8_t> image(m.mem_size, 0);
    std::copy(m.bytes.begin(), m.bytes.end(), image.begin());
    std::vector<std::uint32_t> pending_hi;
    for (const IrxRelocation& r : m.relocs) {
        const std::uint32_t word = word_at(image, r.offset);
        switch (r.type) {
        case r_mips_32:
            put_word(image, r.offset, word + base);
            break;
        case r_mips_26: {
            const std::uint32_t target = ((word & 0x03FFFFFFu) << 2) + base;
            if ((target & 0xF0000000u) != (base & 0xF0000000u)) {
                throw IrxError("a jump at image offset " + hex(r.offset) + " leaves its 256 MiB region");
            }
            put_word(image, r.offset, (word & 0xFC000000u) | ((target >> 2) & 0x03FFFFFFu));
            break;
        }
        case r_mips_hi16:
            pending_hi.push_back(r.offset);
            break;
        case r_mips_lo16: {
            const auto lo = static_cast<std::int32_t>(static_cast<std::int16_t>(word & 0xFFFFu));
            for (const std::uint32_t hi_offset : pending_hi) {
                const std::uint32_t hi_word = word_at(image, hi_offset);
                const std::uint32_t ahl = ((hi_word & 0xFFFFu) << 16) + static_cast<std::uint32_t>(lo);
                const std::uint32_t value = ahl + base;
                put_word(image, hi_offset, (hi_word & 0xFFFF0000u) | (((value + 0x8000u) >> 16) & 0xFFFFu));
            }
            pending_hi.clear();
            put_word(image, r.offset, (word & 0xFFFF0000u) | ((word + base) & 0xFFFFu));
            break;
        }
        default:
            break;
        }
    }
    if (!pending_hi.empty()) {
        throw IrxError("a HI16 relocation at image offset " + hex(pending_hi.front()) +
                       " has no LO16 after it");
    }
    return image;
}

std::string irx_library_name(const IopMemory& ram, std::uint32_t address) {
    std::uint8_t raw[8] = {};
    ram.read(address & (IopMemory::size - 1), raw);
    std::string name;
    for (const std::uint8_t c : raw) {
        if (c == 0) {
            break;
        }
        name.push_back(static_cast<char>(c));
    }
    return name;
}

std::vector<IrxImportTable> find_irx_imports(const IopMemory& ram, std::uint32_t base,
                                             std::uint32_t text_size) {
    std::vector<IrxImportTable> tables;
    const std::uint32_t end = base + (text_size & ~3u);
    for (std::uint32_t at = base; at + irx_table_header + 8 <= end; at += 4) {
        if (ram.read32(at) != irx_import_magic || ram.read32(at + 4) != 0) {
            continue;
        }
        // A word that happens to equal the magic (a constant in the text) is not a table: a table has a
        // printable name and starts with a well-formed stub.
        const std::string name = irx_library_name(ram, at + 12);
        const bool printable = !name.empty() && std::all_of(name.begin(), name.end(),
                                                            [](char c) { return c > 0x20 && c < 0x7F; });
        if (!printable || ram.read32(at + irx_table_header) != irx_stub_jr_ra ||
            (ram.read32(at + irx_table_header + 4) & 0xFFFF0000u) != irx_stub_ordinal) {
            continue;
        }
        IrxImportTable t;
        t.address = at;
        t.library = name;
        t.version = static_cast<std::uint16_t>(ram.read32(at + 8) & 0xFFFFu);
        std::uint32_t stub = at + irx_table_header;
        for (; stub + 8 <= end; stub += 8) {
            const std::uint32_t w0 = ram.read32(stub);
            const std::uint32_t w1 = ram.read32(stub + 4);
            if (w0 == 0 && w1 == 0) {
                break;
            }
            if (w0 != irx_stub_jr_ra || (w1 & 0xFFFF0000u) != irx_stub_ordinal) {
                throw IrxError("import table for " + name + " at " + hex(at) + " has a malformed stub at " +
                               hex(stub));
            }
            t.stubs.push_back(IrxImportStub{stub, static_cast<std::uint16_t>(w1 & 0xFFFFu)});
        }
        if (stub + 8 > end) {
            throw IrxError("import table for " + name + " at " + hex(at) + " runs off the end of the text");
        }
        at = stub + 4; // past the terminator (the loop adds 4)
        tables.push_back(std::move(t));
    }
    return tables;
}

} // namespace dq8::rt
