// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "model/errors.h"
#include "model/executor.h"
#include "model/game_session.h"
#include "model/log_buffer.h"

#include <atomic>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>

// The Diagnostics page's support bundle. The export itself is core's (dq8ctl diagnose: system information,
// toolchain, settings, logs, build manifest; never game data); the launcher only asks for it and shows the
// result.
namespace dq8::launcher {

struct ExportOutcome {
    bool ok = false;
    std::optional<Problem> problem;
    std::string output; // what the exporter printed, for the page's log
};

class DiagnosticsExporter {
  public:
    virtual ~DiagnosticsExporter() = default;
    virtual ExportOutcome write(const std::filesystem::path& file, const std::atomic<bool>& cancel) = 0;
};

// `dq8ctl diagnose --out FILE`, the CLI form of session A's diagnose API (Docs/milestones/M5-launcher.md, A).
std::unique_ptr<DiagnosticsExporter> ctl_diagnostics(std::filesystem::path ctl, ProcessRunner runner = {});

// "dq8-diagnostics-20261022-215800.txt" for a local time.
std::string default_diagnostics_name(std::time_t now);

enum class ExportState { idle, running, done, failed };

class DiagnosticsModel {
  public:
    DiagnosticsModel(std::unique_ptr<DiagnosticsExporter> exporter, ExecutorFactory executors);
    ~DiagnosticsModel();
    DiagnosticsModel(const DiagnosticsModel&) = delete;
    DiagnosticsModel& operator=(const DiagnosticsModel&) = delete;

    bool start(const std::filesystem::path& file); // false while an export runs
    void pump();

    ExportState state() const noexcept { return state_; }
    const std::filesystem::path& file() const noexcept { return file_; }
    const std::optional<Problem>& problem() const noexcept { return problem_; }
    const LogBuffer& log() const noexcept { return log_; }

  private:
    std::unique_ptr<DiagnosticsExporter> exporter_;
    ExecutorFactory executors_;
    std::unique_ptr<Executor> executor_;
    std::shared_ptr<Mailbox<ExportOutcome>> mailbox_ = std::make_shared<Mailbox<ExportOutcome>>();
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
    ExportState state_ = ExportState::idle;
    std::filesystem::path file_;
    std::optional<Problem> problem_;
    LogBuffer log_{500};
};

} // namespace dq8::launcher
