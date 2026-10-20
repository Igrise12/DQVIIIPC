// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/abi/game_module.h"

#include <cstddef>
#include <cstdint>
#include <span>

// One code unit's flat dispatch table (architecture §4.2): one slot per 4 bytes of guest address from `base`
// to `end`, holding the recompiled function that starts there or a resume entry (the owning function, which
// switches on ctx->pc), or null. A view: the module that owns the slots must outlive it.
namespace dq8::rt {

class DispatchTable {
  public:
    DispatchTable() = default;
    // Throws std::invalid_argument unless base and end are 4-aligned, base < end and slots.size() equals
    // (end - base) / 4.
    DispatchTable(std::uint32_t base, std::uint32_t end, std::span<const dq8_guest_fn> slots);

    // The table a validated descriptor describes (validate_descriptor() first; this only re-checks the
    // shape).
    static DispatchTable from_descriptor(const dq8_game_module_desc& desc);

    std::uint32_t base() const noexcept { return base_; }
    std::uint32_t end() const noexcept { return end_; }
    std::size_t slot_count() const noexcept { return slots_.size(); }
    bool empty() const noexcept { return slots_.empty(); }
    bool covers(std::uint32_t addr) const noexcept { return addr >= base_ && addr < end_; }

    // The slot for `addr`, or null when addr is outside the table, not 4-aligned, or the slot is empty.
    dq8_guest_fn lookup(std::uint32_t addr) const noexcept {
        if ((addr & 3u) != 0 || !covers(addr)) {
            return nullptr;
        }
        return slots_[(addr - base_) >> 2];
    }

    // Non-null slots (functions and resume entries). Linear in slot_count().
    std::size_t entry_count() const noexcept;

  private:
    std::uint32_t base_ = 0;
    std::uint32_t end_ = 0;
    std::span<const dq8_guest_fn> slots_;
};

} // namespace dq8::rt
