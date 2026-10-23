// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/region_profile.h"
#include "runtime/machine/machine_services.h"

#include <atomic>
#include <cstdint>
#include <string_view>

// Watches the game's own screen-size option (the profile's [display] table; M5 session C) so that the window
// can follow it (dq8-game --aspect auto): at every vblank start, in the last phase, it reads the variable and
// publishes whether it holds the profile's widescreen value. It only reads guest memory and emits no event,
// so attaching it moves no deterministic output; widescreen() may be read from any thread (the window loop).
// Before the game has loaded its settings the variable is 0 (bss), which reads as 4:3.
namespace dq8::game {

class DisplayWatcher final : public rt::IDevice, private rt::IVblankListener {
  public:
    // Throws std::invalid_argument when the profile has no [display] table.
    explicit DisplayWatcher(const core::RegionProfile::Display& display);

    std::string_view name() const override { return "game.display_watcher"; }
    void attach(rt::MachineServices& machine) override;
    void reset() override;

    bool widescreen() const noexcept { return widescreen_.load(std::memory_order_relaxed); }
    // How often the option changed since reset (tests).
    std::uint64_t changes() const noexcept { return changes_.load(std::memory_order_relaxed); }

  private:
    void on_vblank_start(std::uint64_t field) override;

    core::RegionProfile::Display display_;
    rt::MachineServices* machine_ = nullptr;
    std::atomic<bool> widescreen_{false};
    std::atomic<std::uint64_t> changes_{0};
};

} // namespace dq8::game
