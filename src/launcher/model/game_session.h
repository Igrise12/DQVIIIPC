// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/process.h"
#include "model/errors.h"
#include "model/executor.h"
#include "model/log_buffer.h"
#include "model/status.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Play: the launcher starts dq8-game as a separate process (architecture.md §11.1) through core's run_process
// (an argument vector, never a shell), reads its status lines and keeps what the Play page shows: started,
// first frame, frame rate, warnings, and why it exited.
namespace dq8::launcher {

struct PlayRequest {
    std::filesystem::path game; // the dq8-game executable
    std::filesystem::path iso;
    std::filesystem::path module_dir;
    std::filesystem::path config_file; // the user's settings file; empty: none (M4's defaults)
    std::string profile_id;            // for the page; dq8-game finds the profile from the disc
};

// dq8-game's arguments: --iso, --module-dir, --window, --config (when given), --status - .
std::vector<std::string> game_arguments(const PlayRequest& request);

enum class GamePhase {
    idle,     // nothing started yet
    starting, // the process was asked to start; no "started" line yet
    running,  // "started" arrived
    exited,   // the process has ended; see ExitSummary
};

struct ExitSummary {
    bool ok = false;      // ended as the player asked (window closed) or as the run was told to
    bool stopped = false; // the launcher stopped it
    std::string reason;   // the status line's reason, or empty when none arrived
    std::optional<int> exit_code;
    int signal = 0;
    Problem problem; // ok: a short sentence in message; otherwise what went wrong and what to do
};

// The game's state from the events it reported; no threads, no process (GameSupervisor drives it).
class GameSession {
  public:
    using Clock = std::chrono::steady_clock;

    explicit GameSession(std::size_t log_lines = 2000) : log_(log_lines) {}

    void begin(const PlayRequest& request, Clock::time_point now);
    void on_status(const StatusEvent& event);
    void on_output(std::string_view line); // stderr, and stdout lines that are not status objects
    // The process ended: its result, or the reason it could not start (`spawn_error`).
    void on_finished(const std::optional<core::ProcessResult>& result,
                     const std::optional<Problem>& spawn_error, bool stopped_by_launcher);

    GamePhase phase() const noexcept { return phase_; }
    const PlayRequest& request() const noexcept { return request_; }
    Clock::time_point started_at() const noexcept { return started_at_; }
    const std::optional<StatusStarted>& started() const noexcept { return started_; }
    std::optional<double> first_frame_ms() const noexcept { return first_frame_ms_; }
    const std::optional<StatusFps>& fps() const noexcept { return fps_; }
    const std::vector<StatusWarning>& warnings() const noexcept { return warnings_; }
    const std::optional<StatusExit>& exit_line() const noexcept { return exit_line_; }
    const std::optional<ExitSummary>& exit() const noexcept { return exit_; }
    const LogBuffer& log() const noexcept { return log_; }

  private:
    GamePhase phase_ = GamePhase::idle;
    PlayRequest request_;
    Clock::time_point started_at_{};
    std::optional<StatusStarted> started_;
    std::optional<double> first_frame_ms_;
    std::optional<StatusFps> fps_;
    std::vector<StatusWarning> warnings_;
    std::optional<StatusExit> exit_line_;
    std::optional<ExitSummary> exit_;
    LogBuffer log_;
};

// What the exit line and the process result say, for people. Exposed for tests.
ExitSummary summarize_exit(const std::optional<StatusExit>& line,
                           const std::optional<core::ProcessResult>& result, bool stopped_by_launcher);

using ProcessRunner = std::function<core::ProcessResult(const core::ProcessOptions&)>;

// Runs dq8-game on its executor's thread and feeds its output to a GameSession on the UI thread (pump()).
class GameSupervisor {
  public:
    // `runner` defaults to core::run_process; tests pass a fake.
    explicit GameSupervisor(ExecutorFactory executors, ProcessRunner runner = {});
    ~GameSupervisor();
    GameSupervisor(const GameSupervisor&) = delete;
    GameSupervisor& operator=(const GameSupervisor&) = delete;

    // False (and nothing happens) while a game is starting or running.
    bool start(const PlayRequest& request);
    // Stops the running game and the processes it started.
    void stop();
    // Applies what the worker reported since the last call.
    void pump();

    bool active() const noexcept;
    const GameSession& session() const noexcept { return session_; }

  private:
    struct Finished {
        std::optional<core::ProcessResult> result;
        std::optional<Problem> spawn_error;
    };
    struct Output {
        std::string line;
    };
    using Message = std::variant<StatusEvent, Output, Finished>;

    ExecutorFactory executors_;
    ProcessRunner runner_;
    std::unique_ptr<Executor> executor_;
    std::shared_ptr<Mailbox<Message>> mailbox_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    bool stop_requested_ = false;
    GameSession session_;
};

} // namespace dq8::launcher
