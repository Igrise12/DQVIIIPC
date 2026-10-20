// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/ee/dispatch_table.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The OverlayManager (architecture §4.3) owns the answer to "which overlay is resident?". Every overlay unit
// has its own dispatch table inside one shared address window; the manager routes window addresses to the
// resident unit's table and to nothing when none is resident. The window and the units come from the game
// layer (a region profile); nothing here knows their addresses or names.
namespace dq8::rt {

struct OverlayWindowSpec {
    std::uint32_t base = 0; // first address of the window
    std::uint32_t end = 0;  // one past the last
};

struct OverlayUnit {
    std::uint32_t overlay_id = 0; // non-zero, unique per window
    std::string name;
    DispatchTable table;
};

class OverlayManager {
  public:
    OverlayManager() = default; // no window: every address is outside it
    // Throws std::invalid_argument when base >= end or either is not 4-aligned.
    explicit OverlayManager(OverlayWindowSpec window);

    bool has_window() const noexcept { return window_.has_value(); }
    const std::optional<OverlayWindowSpec>& window() const noexcept { return window_; }
    bool in_window(std::uint32_t addr) const noexcept {
        return window_ && addr >= window_->base && addr < window_->end;
    }

    // Throws std::invalid_argument when there is no window, the id is 0 or already registered, or the table
    // does not lie inside the window. Registering does not make the unit resident.
    void register_unit(std::uint32_t overlay_id, std::string name, DispatchTable table);
    const std::vector<OverlayUnit>& units() const noexcept { return units_; }
    const OverlayUnit* find(std::uint32_t overlay_id) const noexcept;

    // Points the window at a registered unit (throws std::invalid_argument for an unknown id) or at nothing.
    // Each call advances the generation, which is what stale-return detection compares against.
    void set_resident(std::uint32_t overlay_id);
    void clear_resident() noexcept;
    const OverlayUnit* resident() const noexcept;
    std::uint64_t generation() const noexcept { return generation_; }

    // The resident unit's function at `addr`, or null (outside the window, nothing resident, or an empty
    // slot).
    dq8_guest_fn lookup(std::uint32_t addr) const noexcept;

    // Why a window address has no function: what is resident, and which registered units would have one.
    std::string describe_miss(std::uint32_t addr) const;

  private:
    std::optional<OverlayWindowSpec> window_;
    std::vector<OverlayUnit> units_;
    std::optional<std::size_t> resident_; // index into units_
    std::uint64_t generation_ = 0;
};

} // namespace dq8::rt
