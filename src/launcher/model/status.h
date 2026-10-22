// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

// dq8-game's status channel (`--status -`, Docs/milestones/M5-launcher.md "The status channel"): one JSON
// object per line, read here with nlohmann/json. Lines that are not status objects are ordinary output.
namespace dq8::launcher {

struct StatusStarted {
    std::string version;
    std::string profile;
    std::int64_t pid = 0;
    bool window = false;
};

struct StatusFirstFrame {
    double ms = 0.0; // since start
};

struct StatusFps {
    double fps = 0.0;
    double speed = 0.0; // guest time / wall time
};

struct StatusWarning {
    std::string code; // an error-code name or a session-defined kebab name
    std::string message;
};

struct StatusExit {
    std::optional<int> status;
    std::string reason; // window-closed, until, vblanks, stop, error
    std::string code;   // for errors
    std::string message;
};

// A status object with an "ev" this launcher does not know (a later dq8-game); ignored, not shown as output.
struct StatusOther {
    std::string ev;
};

using StatusEvent =
    std::variant<StatusStarted, StatusFirstFrame, StatusFps, StatusWarning, StatusExit, StatusOther>;

// The event on `line`, or nullopt when the line is not a JSON object with a string "ev" (then it is output).
// Fields of the wrong type are left at their defaults rather than rejecting the line.
std::optional<StatusEvent> parse_status_line(std::string_view line);

// Cuts a byte stream that arrives in arbitrary chunks into lines. A line longer than `max_line` is passed on
// in pieces of that size.
class LineSplitter {
  public:
    using LineFn = std::function<void(std::string_view)>;

    explicit LineSplitter(std::size_t max_line = std::size_t{1} << 20)
        : max_line_(max_line == 0 ? 1 : max_line) {}

    void feed(std::string_view chunk, const LineFn& on_line);
    // The unterminated rest, if any, as a last line.
    void finish(const LineFn& on_line);

  private:
    std::size_t max_line_;
    std::string pending_;
};

} // namespace dq8::launcher
