// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/recomp/units.h"

#include "dq8/core/error.h"
#include "dq8/core/intake.h"

#include <algorithm>

namespace dq8::core::recomp {

namespace {

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
    return s;
}

} // namespace

std::string_view to_string(UnitKind kind) noexcept {
    return kind == UnitKind::main ? "main" : "overlay";
}

std::vector<CodeUnit> code_units(const VerifiedImage& v) {
    std::vector<CodeUnit> units;
    const ElfSegment* text = nullptr;
    for (const ElfSegment* s : v.elf.load_segments()) {
        if ((s->flags & elf_pf_x) != 0 && s->filesz != 0) {
            text = s;
            break;
        }
    }
    if (text == nullptr) {
        throw Error(ErrorCode::image_mismatch, "the boot ELF has no executable load segment with data");
    }
    CodeUnit main;
    main.name = "main";
    main.kind = UnitKind::main;
    main.source_path = v.profile.boot.path;
    main.entry = v.elf.entry;
    main.code_start = text->vaddr;
    main.code_end = text->vaddr + text->filesz;
    main.load_address = text->vaddr;
    units.push_back(main);

    // Overlays in name-table order, then any the table does not list in profile order.
    std::vector<std::size_t> order;
    for (const std::string& name : v.profile.overlay_window.name_table_order) {
        for (std::size_t i = 0; i < v.profile.overlays.size(); ++i) {
            if (v.profile.overlays[i].name == name) {
                order.push_back(i);
            }
        }
    }
    for (std::size_t i = 0; i < v.profile.overlays.size(); ++i) {
        if (std::find(order.begin(), order.end(), i) == order.end()) {
            order.push_back(i);
        }
    }
    for (const std::size_t i : order) {
        const ProfileOverlay& row = v.profile.overlays[i];
        if (i >= v.overlay_headers.size()) {
            throw Error(ErrorCode::image_mismatch,
                        "overlay " + row.name + " has no verified container header");
        }
        const Mwo3Header& h = v.overlay_headers[i];
        CodeUnit unit;
        unit.name = lowercase(row.name);
        unit.kind = UnitKind::overlay;
        unit.overlay_id = h.overlay_id;
        unit.source_path = row.path;
        unit.entry = 0;
        unit.code_start = h.load_address;
        unit.code_end = h.bss_start();
        unit.load_address = h.load_address;
        units.push_back(unit);
    }
    return units;
}

std::vector<CodeUnit> select_units(const std::vector<CodeUnit>& units,
                                   const std::vector<std::string>& names) {
    if (names.empty()) {
        return units;
    }
    std::string known;
    for (const CodeUnit& u : units) {
        known += (known.empty() ? "" : ", ") + u.name;
    }
    for (const std::string& n : names) {
        if (std::none_of(units.begin(), units.end(), [&](const CodeUnit& u) { return u.name == n; })) {
            throw Error(ErrorCode::invalid_argument, "unknown unit '" + n + "' (units: " + known + ")");
        }
    }
    std::vector<CodeUnit> out;
    for (const CodeUnit& u : units) {
        if (std::find(names.begin(), names.end(), u.name) != names.end()) {
            out.push_back(u);
        }
    }
    return out;
}

} // namespace dq8::core::recomp
