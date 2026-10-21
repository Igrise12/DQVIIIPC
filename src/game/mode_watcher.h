// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/machine/machine_services.h"

#include <cstdint>
#include <string_view>

// Watches the game-mode variable the main loop switches on (the profile's overlay_window.mode_variable): at
// every vblank start, in the last phase, it reads the 32-bit word and emits `mode` {mode, previous} when the
// value differs from the last one it saw (0 after reset, as bss starts). Sampling at a fixed point of each
// vblank keeps the events deterministic.
namespace dq8::game {

class ModeWatcher final : public rt::IDevice, private rt::IVblankListener {
  public:
    explicit ModeWatcher(std::uint32_t mode_variable);

    std::string_view name() const override { return "game.mode_watcher"; }
    void attach(rt::MachineServices& machine) override;
    void reset() override;

    std::uint32_t mode() const noexcept { return mode_; }
    std::uint64_t changes() const noexcept { return changes_; }

  private:
    void on_vblank_start(std::uint64_t field) override;

    std::uint32_t address_;
    rt::MachineServices* machine_ = nullptr;
    std::uint32_t mode_ = 0;
    std::uint64_t changes_ = 0;
};

} // namespace dq8::game
