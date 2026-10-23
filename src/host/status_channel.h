// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

// The launcher's status channel (dq8-game --status FILE|-; Docs/milestones/M5-launcher.md, "The status
// channel"): one JSON object per line, wall-clock based, never part of a golden and separate from --events.
//   {"ev":"started","version":"0.0.1","profile":"SLUS_21207","pid":1234,"window":true}
//   {"ev":"first_frame","ms":812}
//   {"ev":"fps","fps":59.94,"speed":1.00}          once a wall-clock second
//   {"ev":"warning","code":"settings-invalid","message":"..."}
//   {"ev":"exit","status":0,"reason":"window-closed"} (reason: window-closed, until, vblanks, stop, error;
//                                                      for errors also "code" and "message")
// With "-" the lines go to the process's stdout and everything else dq8-game prints there (the set-up lines,
// the report, --json) goes to stderr, so stdout carries status lines only. Every member may be called from
// any thread.
namespace dq8::host {

class StatusChannel {
  public:
    // `target`: nullopt (no channel), "-" or a file. Throws core::Error(io_write) when the file cannot be
    // created.
    explicit StatusChannel(const std::optional<std::string>& target);
    ~StatusChannel();
    StatusChannel(const StatusChannel&) = delete;
    StatusChannel& operator=(const StatusChannel&) = delete;

    bool enabled() const noexcept { return out_ != nullptr; }

    void started(std::string_view profile, bool window);
    // A frame was shown (the window's present) or, headless, a vblank ended; the first one sends first_frame.
    void frame();
    // The machine thread after each slice: the guest time so far. Sends fps once a wall-clock second.
    void guest_time(double seconds);
    void warning(std::string_view code, std::string_view message);
    // The last line; later calls send nothing.
    void exit(int status, std::string_view reason, std::string_view code = {}, std::string_view message = {});
    // The exit line for an exception run_game lets out, with the status main() turns it into.
    void exit_on(const std::exception_ptr& error);

  private:
    void write_line(const std::string& line);

    using Clock = std::chrono::steady_clock;
    std::mutex mutex_;
    std::FILE* out_ = nullptr;
    bool owns_file_ = false;
    bool exited_ = false;
    Clock::time_point start_ = Clock::now();
    bool first_frame_sent_ = false;
    bool measuring_ = false;
    std::uint64_t frames_ = 0;
    Clock::time_point window_start_ = start_;
    std::uint64_t window_frames_ = 0;
    double window_guest_ = 0.0;
};

// The JSON string literal of `text`, with quotes.
std::string json_string(std::string_view text);

} // namespace dq8::host
