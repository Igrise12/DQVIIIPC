// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/dispatch_table.h"

#include "runtime/ee/guest_stop.h"

#include <stdexcept>
#include <string>

namespace dq8::rt {

DispatchTable::DispatchTable(std::uint32_t base, std::uint32_t end, std::span<const dq8_guest_fn> slots)
    : base_(base), end_(end), slots_(slots) {
    if ((base & 3u) != 0 || (end & 3u) != 0 || base >= end) {
        throw std::invalid_argument("dispatch table range [" + hex32(base) + ", " + hex32(end) +
                                    ") is empty or not 4-aligned");
    }
    if (slots.size() != (end - base) / 4u) {
        throw std::invalid_argument("dispatch table [" + hex32(base) + ", " + hex32(end) + ") needs " +
                                    std::to_string((end - base) / 4u) + " slots, got " +
                                    std::to_string(slots.size()));
    }
}

DispatchTable DispatchTable::from_descriptor(const dq8_game_module_desc& desc) {
    if (desc.table == nullptr) {
        throw std::invalid_argument("descriptor has no table");
    }
    return DispatchTable(desc.table_base, desc.table_end, std::span(desc.table, desc.table_slot_count));
}

std::size_t DispatchTable::entry_count() const noexcept {
    std::size_t n = 0;
    for (const dq8_guest_fn fn : slots_) {
        n += fn != nullptr ? 1u : 0u;
    }
    return n;
}

} // namespace dq8::rt
