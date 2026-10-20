// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/ee/dispatch_table.h"
#include "runtime/ee/overlay_manager.h"

#include <cstdint>
#include <string>
#include <string_view>

// Address -> recompiled function: the main unit's table first, then, for an address in the overlay window,
// the resident overlay's table; anything else is a miss.
namespace dq8::rt {

struct DispatchTarget {
    dq8_guest_fn fn = nullptr;
    std::string_view unit; // the owning unit's name, for traces; empty on a miss
};

class Dispatcher {
  public:
    explicit Dispatcher(const OverlayManager& overlays) : overlays_(overlays) {}

    void set_main(std::string name, DispatchTable table);
    const DispatchTable& main_table() const noexcept { return main_; }
    const std::string& main_name() const noexcept { return main_name_; }
    const OverlayManager& overlays() const noexcept { return overlays_; }

    DispatchTarget resolve(std::uint32_t addr) const noexcept;
    dq8_guest_fn lookup(std::uint32_t addr) const noexcept { return resolve(addr).fn; }

    // One sentence on why `addr` has no function: misaligned, an empty slot of the main table, an overlay
    // window miss (OverlayManager::describe_miss), or outside every table.
    std::string describe_miss(std::uint32_t addr) const;

  private:
    const OverlayManager& overlays_;
    DispatchTable main_;
    std::string main_name_;
};

} // namespace dq8::rt
