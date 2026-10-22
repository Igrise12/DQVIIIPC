// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "model/diagnostics.h"

#include "model/environment.h"

#include <chrono>
#include <utility>

namespace dq8::launcher {

namespace {

class CtlDiagnostics final : public DiagnosticsExporter {
  public:
    CtlDiagnostics(std::filesystem::path ctl, ProcessRunner runner)
        : ctl_(std::move(ctl)), runner_(std::move(runner)) {
        if (!runner_) {
            runner_ = [](const core::ProcessOptions& options) { return core::run_process(options); };
        }
    }

    ExportOutcome write(const std::filesystem::path& file, const std::atomic<bool>& cancel) override {
        ExportOutcome out;
        if (ctl_.empty()) {
            out.problem = explain(core::ErrorCode::tool_missing, "dq8ctl was not found next to the launcher");
            return out;
        }
        core::ProcessOptions options;
        options.program = ctl_;
        options.args = {"diagnose", "--out", to_utf8(file)};
        options.cancel = &cancel;
        options.timeout = std::chrono::minutes(2);
        try {
            const core::ProcessResult r = runner_(options);
            out.output = r.out + r.err;
            if (r.ok()) {
                out.ok = true;
                return out;
            }
            Problem p;
            p.message = r.cancelled ? "The export was cancelled."
                        : r.timed_out
                            ? "dq8ctl diagnose did not finish in time."
                            : "dq8ctl diagnose failed (status " + std::to_string(r.exit_code) + ").";
            p.action = "Read its output below.";
            out.problem = std::move(p);
        } catch (...) {
            out.problem = explain_current_exception();
        }
        return out;
    }

  private:
    std::filesystem::path ctl_;
    ProcessRunner runner_;
};

} // namespace

std::unique_ptr<DiagnosticsExporter> ctl_diagnostics(std::filesystem::path ctl, ProcessRunner runner) {
    return std::make_unique<CtlDiagnostics>(std::move(ctl), std::move(runner));
}

std::string default_diagnostics_name(std::time_t now) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[64];
    const std::size_t n = std::strftime(buf, sizeof(buf), "dq8-diagnostics-%Y%m%d-%H%M%S.txt", &tm);
    return n != 0 ? std::string(buf, n) : std::string("dq8-diagnostics.txt");
}

DiagnosticsModel::DiagnosticsModel(std::unique_ptr<DiagnosticsExporter> exporter, ExecutorFactory executors)
    : exporter_(std::move(exporter)), executors_(std::move(executors)) {}

DiagnosticsModel::~DiagnosticsModel() {
    cancel_->store(true);
    executor_.reset();
}

bool DiagnosticsModel::start(const std::filesystem::path& file) {
    if (state_ == ExportState::running || file.empty()) {
        return false;
    }
    if (!executor_) {
        executor_ = executors_();
    }
    state_ = ExportState::running;
    file_ = file;
    problem_.reset();
    log_.add("Exporting diagnostics to " + to_utf8(file));
    executor_->run([exporter = exporter_.get(), mailbox = mailbox_, cancel = cancel_, file] {
        mailbox->post(exporter->write(file, *cancel));
    });
    return true;
}

void DiagnosticsModel::pump() {
    for (ExportOutcome& outcome : mailbox_->take()) {
        if (!outcome.output.empty()) {
            log_.add(outcome.output);
        }
        if (outcome.ok) {
            state_ = ExportState::done;
            log_.add("Written: " + to_utf8(file_));
        } else {
            state_ = ExportState::failed;
            problem_ = std::move(outcome.problem);
            if (problem_) {
                log_.add("Failed: " + problem_->message);
            }
        }
    }
}

} // namespace dq8::launcher
