// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "model/setup_progress.h"

#include <algorithm>

namespace dq8::launcher {

namespace {

using core::setup::Stage;
using core::setup::State;

// Each stage's rough share of a first setup's time, in percent (architecture.md §3 "Time and disk": the
// module build dominates, the smoke test boots to the title). Only the bar's pacing depends on these.
constexpr std::array<double, stage_count> stage_weight = {1, 2, 1, 1, 2, 3, 80, 8, 2};

} // namespace

Stage stage_at(std::size_t index) noexcept {
    return static_cast<Stage>(std::min(index, stage_count - 1));
}

std::size_t stage_index(Stage stage) noexcept {
    const auto i = static_cast<std::size_t>(stage);
    return std::min(i, stage_count - 1);
}

std::string_view stage_title(Stage stage) noexcept {
    switch (stage) {
    case Stage::verify:
        return "Verify the disc image";
    case Stage::extract:
        return "Copy the game's code from the disc";
    case Stage::toolchain:
        return "Check the build tools";
    case Stage::maps:
        return "Load the function maps";
    case Stage::prepare:
        return "Prepare the code units";
    case Stage::generate:
        return "Translate the code to C++";
    case Stage::build:
        return "Build the game modules";
    case Stage::smoke:
        return "Test the new build";
    case Stage::register_install:
        return "Register the build";
    }
    return "Setup";
}

std::string format_event(const core::setup::Event& event) {
    std::string line = "[" + std::string(core::setup::to_string(event.stage)) + "] ";
    switch (event.state) {
    case State::start:
        line += "started";
        break;
    case State::progress:
        break;
    case State::done:
        line += "done";
        break;
    case State::skipped:
        line += "up to date, skipped";
        break;
    case State::error:
        line += "failed";
        if (event.code) {
            line += " (" + std::string(core::to_string(*event.code)) + ")";
        }
        break;
    }
    const bool has_head = event.state != State::progress;
    if (!event.unit.empty()) {
        line += (has_head ? ", " : "") + event.unit;
    }
    if (!event.message.empty()) {
        line += (has_head || !event.unit.empty() ? ": " : "") + event.message;
    }
    if (event.state == State::progress && event.total != 0) {
        line += " (" + std::to_string(event.done) + "/" + std::to_string(event.total) + ")";
    }
    return line;
}

std::optional<double> StageProgress::fraction() const noexcept {
    if (state == StageState::done || state == StageState::skipped) {
        return 1.0;
    }
    if (total == 0) {
        return std::nullopt;
    }
    return std::clamp(static_cast<double>(done) / static_cast<double>(total), 0.0, 1.0);
}

void SetupProgress::reset() {
    for (std::size_t i = 0; i < stage_count; ++i) {
        stages_[i] = StageProgress{};
        stages_[i].stage = stage_at(i);
    }
}

void SetupProgress::apply(const core::setup::Event& event, Clock::time_point now) {
    StageProgress& s = stages_[stage_index(event.stage)];
    switch (event.state) {
    case State::start:
        s.state = StageState::running;
        s.done = 0;
        s.total = event.total;
        s.code.reset();
        s.started = now;
        break;
    case State::progress:
        if (s.state == StageState::pending) { // a progress without its start still counts
            s.state = StageState::running;
            s.started = now;
        }
        s.done = event.done;
        s.total = event.total;
        break;
    case State::done:
        s.state = StageState::done;
        s.done = event.total != 0 ? event.total : std::max(s.done, event.done);
        s.total = event.total != 0 ? event.total : s.total;
        s.finished = now;
        break;
    case State::skipped:
        s.state = StageState::skipped;
        s.finished = now;
        break;
    case State::error:
        s.state = StageState::failed;
        s.code = event.code;
        s.finished = now;
        break;
    }
    if (!event.unit.empty()) {
        s.unit = event.unit;
    }
    if (!event.message.empty()) {
        s.message = event.message;
    }
}

std::optional<core::setup::Stage> SetupProgress::current() const noexcept {
    std::optional<Stage> found;
    Clock::time_point latest{};
    for (const StageProgress& s : stages_) {
        if (s.state == StageState::running && (!found || s.started >= latest)) {
            found = s.stage;
            latest = s.started;
        }
    }
    return found;
}

double SetupProgress::overall() const noexcept {
    double sum = 0.0;
    double weight = 0.0;
    for (std::size_t i = 0; i < stage_count; ++i) {
        weight += stage_weight[i];
        const StageProgress& s = stages_[i];
        if (s.state == StageState::pending) {
            continue;
        }
        sum += stage_weight[i] * s.fraction().value_or(0.0);
    }
    return weight > 0.0 ? std::clamp(sum / weight, 0.0, 1.0) : 0.0;
}

std::optional<std::chrono::seconds> SetupProgress::remaining(Clock::time_point now) const noexcept {
    const auto stage = current();
    if (!stage) {
        return std::nullopt;
    }
    const StageProgress& s = stages_[stage_index(*stage)];
    if (s.total == 0 || s.done == 0 || s.done >= s.total || now <= s.started) {
        return std::nullopt;
    }
    const double elapsed = std::chrono::duration<double>(now - s.started).count();
    const double left = elapsed * static_cast<double>(s.total - s.done) / static_cast<double>(s.done);
    return std::chrono::seconds(static_cast<std::chrono::seconds::rep>(left + 0.5));
}

} // namespace dq8::launcher
