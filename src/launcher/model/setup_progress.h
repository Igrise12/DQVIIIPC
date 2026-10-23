// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/error.h"
#include "dq8/core/setup.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The setup's progress as the wizard shows it: one row per stage (setup.h's nine, in their order), built from
// setup::Event values (Docs/milestones/M5-launcher.md "The setup's events").
namespace dq8::launcher {

inline constexpr std::size_t stage_count = 9;

core::setup::Stage stage_at(std::size_t index) noexcept;
std::size_t stage_index(core::setup::Stage stage) noexcept;
// "Verify the disc image", "Build the game modules", ...
std::string_view stage_title(core::setup::Stage stage) noexcept;
// One log line for an event: "[build] main: message (120/845)".
std::string format_event(const core::setup::Event& event);

enum class StageState { pending, running, done, skipped, failed };

struct StageProgress {
    core::setup::Stage stage = core::setup::Stage::verify;
    StageState state = StageState::pending;
    std::uint64_t done = 0;
    std::uint64_t total = 0; // 0: unknown
    std::string unit;
    std::string message;
    std::optional<core::ErrorCode> code;
    std::chrono::steady_clock::time_point started{};
    std::chrono::steady_clock::time_point finished{};

    // done / total in [0, 1], or nullopt when the total is unknown.
    std::optional<double> fraction() const noexcept;
};

class SetupProgress {
  public:
    using Clock = std::chrono::steady_clock;

    SetupProgress() { reset(); }
    void reset();
    void apply(const core::setup::Event& event, Clock::time_point now);

    const std::array<StageProgress, stage_count>& stages() const noexcept { return stages_; }
    const StageProgress& stage(core::setup::Stage stage) const noexcept {
        return stages_[stage_index(stage)];
    }
    // The stage that started last and has not finished.
    std::optional<core::setup::Stage> current() const noexcept;
    // 0 to 1 over the whole setup: each stage weighs its usual share of a first run's time (the build most).
    double overall() const noexcept;
    // The current stage's remaining time at its rate so far, when its total is known and some work is done.
    std::optional<std::chrono::seconds> remaining(Clock::time_point now) const noexcept;

  private:
    std::array<StageProgress, stage_count> stages_{};
};

} // namespace dq8::launcher
