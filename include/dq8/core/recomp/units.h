// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Code units: the pieces of the game the recompiler sees separately (architecture.md §4.3). The boot ELF is
// unit "main"; every overlay of the region profile is a unit named after its profile row, lowercased
// ("title"). Units are ordered main first, then the overlays in the loader's name-table order.
namespace dq8::core {
struct VerifiedImage;
}

namespace dq8::core::recomp {

enum class UnitKind { main, overlay };

std::string_view to_string(UnitKind kind) noexcept; // "main" | "overlay"

struct CodeUnit {
    std::string name;
    UnitKind kind = UnitKind::main;
    std::uint32_t overlay_id = 0; // the container's overlay_id; 0 for main
    std::string source_path;      // the disc path of the boot file or the overlay file
    std::uint32_t entry = 0;      // e_entry of the unit ELF: the boot entry for main, 0 for overlays
    // Where map functions may live: main's executable load segment, or an overlay's whole file range
    // [load_address, load_address + 64 + text + data) (code lies in the data range too; spike S3).
    std::uint32_t code_start = 0;
    std::uint32_t code_end = 0;
    std::uint32_t load_address = 0; // the overlay's load address (the window base); main: its segment's vaddr
};

// The verified disc's code units, in pipeline order. Throws Error(image_mismatch) when the boot ELF has no
// executable load segment.
std::vector<CodeUnit> code_units(const VerifiedImage& verified);

// The units named in `names` (all of them when `names` is empty), in pipeline order. Throws
// Error(invalid_argument) naming any unknown unit and the valid names.
std::vector<CodeUnit> select_units(const std::vector<CodeUnit>& units, const std::vector<std::string>& names);

} // namespace dq8::core::recomp
