// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/abi/game_module.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// One code unit's flat dispatch table (architecture §4.2): one slot per 4 bytes of guest address from `base`
// to `end`, holding the recompiled function that starts there or a resume entry (the owning function, which
// switches on ctx->pc), or null. Either a view (the module that owns the slots must outlive it) or an owned
// copy (owned_copy()), which the dispatcher and the overlay manager keep so that overrides can repoint single
// slots (overrides.h); copies of an owned table share its slots. Function pointers in either form point into
// the module, so the module must outlive both.
namespace dq8::rt {

class DispatchTable {
  public:
    DispatchTable() = default;
    // Throws std::invalid_argument unless base and end are 4-aligned, base < end and slots.size() equals
    // (end - base) / 4.
    DispatchTable(std::uint32_t base, std::uint32_t end, std::span<const dq8_guest_fn> slots);

    // The table a validated descriptor describes (validate_descriptor() first; this only re-checks the
    // shape). A view.
    static DispatchTable from_descriptor(const dq8_game_module_desc& desc);

    // The same slots in storage of its own. An empty table copies to an empty table.
    DispatchTable owned_copy() const;
    bool owned() const noexcept { return storage_ != nullptr; }

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

    // True when `addr` holds a function's own entry rather than a resume entry: the slot is non-null and no
    // lower slot of the table holds the same function (a resume entry points at its owning function, whose
    // entry lies below it). Linear in the distance from the table base.
    bool is_function_entry(std::uint32_t addr) const noexcept;

    // Non-null slots (functions and resume entries). Linear in slot_count().
    std::size_t entry_count() const noexcept;

  private:
    friend class Dispatcher;
    // Owned tables only (the dispatcher's override installation): points the slot for `addr` at `fn` in the
    // shared storage and returns what it held. The table object itself does not change, hence const.
    dq8_guest_fn replace_slot(std::uint32_t addr, dq8_guest_fn fn) const;

    std::uint32_t base_ = 0;
    std::uint32_t end_ = 0;
    std::span<const dq8_guest_fn> slots_;
    std::shared_ptr<std::vector<dq8_guest_fn>> storage_; // set for an owned copy; slots_ views it
};

} // namespace dq8::rt
