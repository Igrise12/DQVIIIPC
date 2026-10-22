// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace dq8::launcher {

// The last `capacity` lines of a log (the setup's events, the game's output), for the log views and the
// diagnostics page. Not thread-safe: view-models fill it on the UI thread.
class LogBuffer {
  public:
    explicit LogBuffer(std::size_t capacity = 4000) : capacity_(capacity == 0 ? 1 : capacity) {}

    // One line; a trailing "\r" or "\n" is dropped, embedded newlines split it.
    void add(std::string_view line);
    void clear();

    const std::deque<std::string>& lines() const noexcept { return lines_; }
    // Lines dropped from the front since the last clear().
    std::uint64_t dropped() const noexcept { return dropped_; }
    // Grows with every change, so a view can scroll to the end when it moves.
    std::uint64_t generation() const noexcept { return generation_; }
    // The lines joined with "\n" (copy to clipboard, export).
    std::string text() const;

  private:
    std::size_t capacity_;
    std::deque<std::string> lines_;
    std::uint64_t dropped_ = 0;
    std::uint64_t generation_ = 0;
};

} // namespace dq8::launcher
