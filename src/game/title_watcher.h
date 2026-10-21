// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/region_profile.h"
#include "runtime/machine/machine_services.h"

#include <cstdint>
#include <string_view>

// Watches for the title screen as the profile's [title] marker describes it: at every vblank start, in the
// last phase, the overlay id in the window header must be the [title] overlay's and the marker variable must
// hold its value. The first vblank that holds emits `title` {field}; dq8-game --until title ends the run
// there.
namespace dq8::game {

class TitleWatcher final : public rt::IDevice, private rt::IVblankListener {
  public:
    // Throws std::invalid_argument when the profile has no [title] marker.
    explicit TitleWatcher(const core::RegionProfile& profile);

    std::string_view name() const override { return "game.title_watcher"; }
    void attach(rt::MachineServices& machine) override;
    void reset() override;

    bool seen() const noexcept { return seen_; }

  private:
    void on_vblank_start(std::uint64_t field) override;

    std::uint32_t id_address_; // window base + 4: the resident overlay's id in its header
    std::uint32_t overlay_id_;
    std::uint32_t variable_;
    std::uint32_t width_;
    std::uint32_t value_;
    rt::MachineServices* machine_ = nullptr;
    bool seen_ = false;
};

} // namespace dq8::game
