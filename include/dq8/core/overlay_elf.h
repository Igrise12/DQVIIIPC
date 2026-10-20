// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/mwo3.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The unit ELF of one overlay: what the recompiler reads for a code unit that is not the boot ELF (the boot
// ELF's six window segments carry no bytes, so the recompiler cannot see overlay code through it; spike S1).
//
// One PROGBITS section ".text" (SHF_ALLOC | SHF_EXECINSTR | SHF_WRITE) holds the whole container file, header
// included, at load_address: that is exactly what the game's loader puts in memory (spike S3), and since the
// text/data split is a link-map boundary rather than a code boundary, code in the data range (constructor
// thunks) stays decodable. A NOBITS ".bss" covers bss_size bytes after it when bss_size is non-zero. e_entry
// is 0 (an overlay has no entry point of its own), e_flags is the boot ELF's, and there are no symbols:
// function boundaries come from the unit's function map.
namespace dq8::core {

struct OverlayUnitElf {
    Mwo3Header header; // the container header the ELF was built from
    std::vector<std::uint8_t> elf;
};

// Parses `container` (a whole MWo3 file) against `window` when given, then builds the unit ELF.
// Throws the parser's Error codes (mwo3_*) or elf_layout.
OverlayUnitElf make_overlay_unit_elf(std::span<const std::uint8_t> container, std::uint32_t elf_flags,
                                     std::optional<Mwo3Window> window = std::nullopt);

} // namespace dq8::core
