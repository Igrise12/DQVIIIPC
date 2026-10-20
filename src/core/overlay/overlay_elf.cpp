// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/overlay_elf.h"

#include "dq8/core/elf_writer.h"

namespace dq8::core {

namespace {

// The largest power of two up to 16 that divides `address`: the section alignment the address allows.
std::uint32_t natural_align(std::uint32_t address) noexcept {
    std::uint32_t align = 16;
    while (align > 1 && address % align != 0) {
        align /= 2;
    }
    return align;
}

} // namespace

OverlayUnitElf make_overlay_unit_elf(std::span<const std::uint8_t> container, std::uint32_t elf_flags,
                                     std::optional<Mwo3Window> window) {
    const Mwo3Overlay overlay = parse_mwo3(container, window);
    const Mwo3Header& h = overlay.header;

    ElfWriter writer;
    writer.set_entry(0).set_flags(elf_flags);
    ElfSection text;
    text.name = ".text";
    text.type = elf_sht_progbits;
    text.flags = elf_shf_alloc | elf_shf_execinstr | elf_shf_write;
    text.address = h.load_address;
    text.align = natural_align(h.load_address);
    text.data.assign(container.begin(), container.begin() + h.file_size());
    writer.add_section(std::move(text));
    if (h.bss_size != 0) {
        ElfSection bss;
        bss.name = ".bss";
        bss.type = elf_sht_nobits;
        bss.flags = elf_shf_alloc | elf_shf_write;
        bss.address = h.bss_start();
        bss.align = natural_align(h.bss_start());
        bss.nobits_size = h.bss_size;
        writer.add_section(std::move(bss));
    }
    return OverlayUnitElf{h, writer.build()};
}

} // namespace dq8::core
