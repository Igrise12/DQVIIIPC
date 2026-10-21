// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/io/pad_state.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>

// Live input (ADR-0005): the main thread publishes the platform's pad states as often as it likes; the
// machine thread latches both ports once per vblank, on the first sample of that vblank, so every port read
// within one vblank sees the same moment. Live runs are not reproducible by nature; --record-input
// (input_record.h) writes what was latched as a script that replays them.
namespace dq8::rt {

class LiveInput final : public IInputSource {
  public:
    // Main thread: the newest state of `port` (0 or 1).
    void publish(unsigned port, const PadState& state);

    // Machine thread.
    PadState sample(unsigned port, std::uint64_t vblank) override;

  private:
    std::mutex mutex_;
    std::array<PadState, 2> published_{};
    std::array<PadState, 2> latched_{};
    std::optional<std::uint64_t> latched_vblank_;
};

// No input: both ports connected with nothing pressed (headless runs without a script).
class IdleInput final : public IInputSource {
  public:
    PadState sample(unsigned /*port*/, std::uint64_t /*vblank*/) override { return PadState{}; }
};

} // namespace dq8::rt
