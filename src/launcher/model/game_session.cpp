// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "model/game_session.h"

#include "model/environment.h"

#include <type_traits>
#include <utility>

namespace dq8::launcher {

std::vector<std::string> game_arguments(const PlayRequest& request) {
    std::vector<std::string> args{"--iso", to_utf8(request.iso), "--module-dir", to_utf8(request.module_dir),
                                  "--window"};
    if (!request.config_file.empty()) {
        args.emplace_back("--config");
        args.push_back(to_utf8(request.config_file));
    }
    args.emplace_back("--status");
    args.emplace_back("-");
    return args;
}

void GameSession::begin(const PlayRequest& request, Clock::time_point now) {
    phase_ = GamePhase::starting;
    request_ = request;
    started_at_ = now;
    started_.reset();
    first_frame_ms_.reset();
    fps_.reset();
    warnings_.clear();
    exit_line_.reset();
    exit_.reset();
    log_.clear();
}

void GameSession::on_status(const StatusEvent& event) {
    std::visit(
        [this](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, StatusStarted>) {
                started_ = e;
                if (phase_ == GamePhase::starting) {
                    phase_ = GamePhase::running;
                }
            } else if constexpr (std::is_same_v<T, StatusFirstFrame>) {
                first_frame_ms_ = e.ms;
            } else if constexpr (std::is_same_v<T, StatusFps>) {
                fps_ = e;
            } else if constexpr (std::is_same_v<T, StatusWarning>) {
                warnings_.push_back(e);
                log_.add("warning: " + (e.code.empty() ? std::string() : e.code + ": ") + e.message);
            } else if constexpr (std::is_same_v<T, StatusExit>) {
                exit_line_ = e;
            }
        },
        event);
}

void GameSession::on_output(std::string_view line) {
    log_.add(line);
}

void GameSession::on_finished(const std::optional<core::ProcessResult>& result,
                              const std::optional<Problem>& spawn_error, bool stopped_by_launcher) {
    phase_ = GamePhase::exited;
    if (spawn_error) {
        ExitSummary s;
        s.problem = *spawn_error;
        exit_ = std::move(s);
        return;
    }
    exit_ = summarize_exit(exit_line_, result, stopped_by_launcher);
}

ExitSummary summarize_exit(const std::optional<StatusExit>& line,
                           const std::optional<core::ProcessResult>& result, bool stopped_by_launcher) {
    ExitSummary s;
    if (result) {
        if (result->exit_code >= 0 && result->signal == 0) {
            s.exit_code = result->exit_code;
        }
        s.signal = result->signal;
    }
    if (line) {
        s.reason = line->reason;
        if (!s.exit_code && line->status) {
            s.exit_code = line->status;
        }
    }
    if (stopped_by_launcher || (result && result->cancelled)) {
        s.ok = true;
        s.stopped = true;
        s.problem.message = "The game was stopped from the launcher.";
        return s;
    }
    if (line) {
        if (line->reason == "error") {
            s.problem = explain_name(line->code, line->message.empty() ? "The game stopped with an error."
                                                                       : std::string_view(line->message));
            if (s.problem.code) {
                s.problem.detail = line->message;
            }
            return s;
        }
        if (line->reason == "window-closed") {
            s.ok = true;
            s.problem.message = "The game was closed.";
            return s;
        }
        if (line->reason == "until" || line->reason == "vblanks" || line->reason == "stop") {
            s.ok = true;
            s.problem.message = "The run ended as it was asked to (" + line->reason + ").";
            return s;
        }
    }
    if (s.signal != 0) {
        s.problem.message = "The game stopped unexpectedly (signal " + std::to_string(s.signal) + ").";
        s.problem.action =
            "Export diagnostics from the Diagnostics page and report the problem with the game's "
            "log below.";
        return s;
    }
    if (!s.exit_code) {
        s.problem.message = "The game ended without reporting why.";
        s.problem.action = "Read the last lines of the game's log below.";
        return s;
    }
    switch (*s.exit_code) {
    case 0:
        s.ok = true;
        s.problem.message = "The game ended.";
        break;
    case 1:
        s.problem.message = "The game reached a point the port does not handle yet and stopped.";
        s.problem.action = "Export diagnostics from the Diagnostics page and report the problem.";
        break;
    case 2:
        s.problem.message =
            "The game refused to start: an option, a file, the disc image or the game modules "
            "were rejected.";
        s.problem.action =
            "Read the last lines of the game's log below. If the modules are out of date, run the "
            "setup again.";
        break;
    case 3:
        s.problem.message = "The game's graphics or its window failed to start.";
        s.problem.action = "Make sure the graphics driver supports Vulkan and is up to date.";
        break;
    case 4:
        s.problem.message = "The run ended before its goal was reached.";
        break;
    default:
        s.problem.message = "The game ended with status " + std::to_string(*s.exit_code) + ".";
        s.problem.action = "Read the last lines of the game's log below.";
        break;
    }
    return s;
}

GameSupervisor::GameSupervisor(ExecutorFactory executors, ProcessRunner runner)
    : executors_(std::move(executors)), runner_(std::move(runner)) {
    if (!runner_) {
        runner_ = [](const core::ProcessOptions& options) { return core::run_process(options); };
    }
}

GameSupervisor::~GameSupervisor() {
    if (cancel_) {
        cancel_->store(true);
    }
    executor_.reset(); // waits for the worker
}

bool GameSupervisor::active() const noexcept {
    return session_.phase() == GamePhase::starting || session_.phase() == GamePhase::running;
}

bool GameSupervisor::start(const PlayRequest& request) {
    if (active()) {
        return false;
    }
    if (!executor_) {
        executor_ = executors_();
    }
    session_.begin(request, GameSession::Clock::now());
    stop_requested_ = false;
    // Fresh per run, shared with the worker, so a late message from a previous run can never reach this one.
    mailbox_ = std::make_shared<Mailbox<Message>>();
    cancel_ = std::make_shared<std::atomic<bool>>(false);

    core::ProcessOptions options;
    options.program = request.game;
    options.args = game_arguments(request);
    options.cancel = cancel_.get();
    options.capture_limit = std::size_t{256} << 10; // the session's log keeps what the page shows
    executor_->run([runner = runner_, options = std::move(options), mailbox = mailbox_,
                    cancel = cancel_]() mutable {
        LineSplitter out;
        LineSplitter err;
        const auto on_out = [&mailbox](std::string_view line) {
            if (auto event = parse_status_line(line)) {
                mailbox->post(std::move(*event));
            } else {
                mailbox->post(Output{std::string(line)});
            }
        };
        const auto on_err = [&mailbox](std::string_view line) { mailbox->post(Output{std::string(line)}); };
        options.on_output = [&](core::OutputStream stream, std::string_view chunk) {
            if (stream == core::OutputStream::out) {
                out.feed(chunk, on_out);
            } else {
                err.feed(chunk, on_err);
            }
        };
        Finished finished;
        try {
            finished.result = runner(options);
        } catch (...) {
            finished.spawn_error = explain_current_exception();
        }
        out.finish(on_out);
        err.finish(on_err);
        mailbox->post(std::move(finished));
        (void)cancel; // kept alive for options.cancel until the runner returned
    });
    return true;
}

void GameSupervisor::stop() {
    if (active() && cancel_) {
        stop_requested_ = true;
        cancel_->store(true);
    }
}

void GameSupervisor::pump() {
    if (!mailbox_) {
        return;
    }
    for (Message& message : mailbox_->take()) {
        if (auto* event = std::get_if<StatusEvent>(&message)) {
            session_.on_status(*event);
        } else if (auto* output = std::get_if<Output>(&message)) {
            session_.on_output(output->line);
        } else if (auto* finished = std::get_if<Finished>(&message)) {
            session_.on_finished(finished->result, finished->spawn_error, stop_requested_);
        }
    }
}

} // namespace dq8::launcher
