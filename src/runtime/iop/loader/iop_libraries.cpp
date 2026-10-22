// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/loader/iop_libraries.h"

#include "runtime/iop/iop_memory.h"
#include "runtime/iop/loader/irx_image.h"

#include <utility>

namespace dq8::rt {

namespace {

// More entries than any export table holds: a missing terminator.
constexpr std::size_t max_exports = 1024;

} // namespace

void IopLibraries::add_hle(std::string name, std::map<std::uint16_t, std::uint32_t> slots) {
    hle_[std::move(name)] = std::move(slots);
}

bool IopLibraries::is_hle(std::string_view name) const {
    return hle_.find(name) != hle_.end();
}

std::int32_t IopLibraries::register_guest(const IopMemory& ram, std::uint32_t table, std::string module) {
    const std::uint32_t at = table & (IopMemory::size - 1);
    if (at + irx_table_header + 4 > IopMemory::size || ram.read32(at) != irx_export_magic) {
        return iop_error::illegal_library;
    }
    GuestLibrary lib;
    lib.table = at;
    lib.name = irx_library_name(ram, at + 12);
    lib.version = static_cast<std::uint16_t>(ram.read32(at + 8) & 0xFFFFu);
    lib.module = std::move(module);
    if (lib.name.empty() || is_hle(lib.name)) {
        return lib.name.empty() ? iop_error::illegal_library : iop_error::library_found;
    }
    for (const auto& [addr, other] : guest_) {
        if (other.name == lib.name && (other.version >> 8) == (lib.version >> 8)) {
            return iop_error::library_found;
        }
    }
    for (std::uint32_t p = at + irx_table_header; p + 4 <= IopMemory::size; p += 4) {
        const std::uint32_t fn = ram.read32(p);
        if (fn == 0) {
            break;
        }
        lib.functions.push_back(fn);
        if (lib.functions.size() > max_exports) {
            return iop_error::illegal_library;
        }
    }
    guest_[at] = std::move(lib);
    return iop_error::ok;
}

std::int32_t IopLibraries::release_guest(std::uint32_t table) {
    return guest_.erase(table & (IopMemory::size - 1)) != 0 ? iop_error::ok : iop_error::library_notfound;
}

IopLinkTarget IopLibraries::resolve(std::string_view library, std::uint16_t version,
                                    std::uint16_t ordinal) const {
    IopLinkTarget t;
    if (const auto it = hle_.find(library); it != hle_.end()) {
        const auto slot = it->second.find(ordinal);
        if (slot == it->second.end()) {
            t.kind = IopLinkTarget::Kind::unimplemented;
        } else {
            t.kind = IopLinkTarget::Kind::hle;
            t.slot = slot->second;
        }
        return t;
    }
    for (const auto& [addr, lib] : guest_) {
        if (lib.name != library || (lib.version >> 8) != (version >> 8)) {
            continue;
        }
        if (ordinal < lib.functions.size()) {
            t.kind = IopLinkTarget::Kind::guest;
            t.address = lib.functions[ordinal];
        } else {
            t.kind = IopLinkTarget::Kind::unimplemented;
        }
        return t;
    }
    t.kind = IopLinkTarget::Kind::missing_library;
    return t;
}

} // namespace dq8::rt
