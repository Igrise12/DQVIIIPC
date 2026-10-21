// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/io/pad_state.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// --record-input: wraps the run's input source and writes every change of a port's state as a `.dq8in`
// line at its absolute vblank (`N [p2] state BUTTONS [lx=..]`, `disconnect`, `connect`), so the file replays
// the run through ScriptInput. Buttons and sticks are recorded; pressure is not (scripts derive it).
namespace dq8::rt {

class RecordingInput final : public IInputSource {
  public:
    // Throws std::runtime_error when the file cannot be created. `inner` must outlive this object.
    RecordingInput(IInputSource& inner, const std::filesystem::path& file);
    ~RecordingInput() override;

    PadState sample(unsigned port, std::uint64_t vblank) override;

    // The `.dq8in` lines (no newlines) that take a port from `previous` (nullopt: a script's start state,
    // connected, nothing pressed, sticks centred) to `state` at `vblank`; empty when nothing a script
    // records changed. Exposed for tests.
    static std::vector<std::string> format_changes(unsigned port, std::uint64_t vblank, const PadState& state,
                                                   const std::optional<PadState>& previous);

    void flush();
    std::uint64_t lines() const noexcept { return lines_; }

  private:
    IInputSource& inner_;
    std::ofstream out_;
    std::array<std::optional<PadState>, 2> last_{};
    std::uint64_t lines_ = 0;
};

} // namespace dq8::rt
