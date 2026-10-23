// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/setup.h"
#include "dq8/core/toolchain.h"
#include "model/errors.h"
#include "model/executor.h"
#include "model/log_buffer.h"
#include "model/setup_progress.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// The first-run wizard (architecture.md §11.2): choose the disc image, verify it, check the toolchain, run
// the setup with per-stage progress and a log, cancel and resume. A state machine over a WizardBackend
// (core's intake, toolchain and setup APIs; fakes in tests); the slow calls run on the wizard's executor.
namespace dq8::launcher {

struct VerifySummary {
    std::string profile_id; // "SLUS_21207"
    std::string title;      // the region profile's title
    std::string boot_sha1;
    std::uint64_t image_size = 0;
    std::vector<std::string> warnings;
};

struct ToolchainReport {
    core::toolchain::Toolchain toolchain;
    std::vector<core::toolchain::Problem> problems; // empty: ready
};

class WizardBackend {
  public:
    virtual ~WizardBackend() = default;
    // Throws core::Error (unknown_image, image_mismatch, ...).
    virtual VerifySummary verify(const std::filesystem::path& iso) = 0;
    // detect() and check(); throws when detection itself fails.
    virtual ToolchainReport check_toolchain() = 0;
    virtual unsigned default_jobs() = 0;
    // setup::run(): setup failures come back in the Result, not as exceptions.
    virtual core::setup::Result run_setup(const core::setup::Options& options) = 0;
};

enum class WizardStep {
    choose_image,       // pick the disc image
    verifying,          // checking it against the region profiles
    verify_failed,      // not a supported disc (problem())
    toolchain_checking, // looking for the compiler, cmake and ninja
    toolchain,          // the check found problems or failed; advice shown, check again or continue
    ready,              // verified and the toolchain is complete
    running,            // setup::run() on the worker
    cancelling,         // cancel asked; waiting for the setup to stop at a safe point
    cancelled,          // stopped by the user; resume() continues where it stopped
    failed,             // the setup ended with an error (problem()); resume() retries
    finished,           // installed and registered
};

class Wizard {
  public:
    using Clock = std::chrono::steady_clock;

    // `executable_dir` is passed to setup::Options (where dq8-game, ps2_recomp and dq8ctl are).
    Wizard(WizardBackend& backend, ExecutorFactory executors, std::filesystem::path executable_dir,
           std::function<Clock::time_point()> clock = {});
    ~Wizard();
    Wizard(const Wizard&) = delete;
    Wizard& operator=(const Wizard&) = delete;

    // setup::Options::game_executable: the dq8-game the launcher starts, so the smoke test runs the same one.
    void set_game_executable(std::filesystem::path game) { game_executable_ = std::move(game); }

    // choose_image, verify_failed, toolchain, ready, cancelled, failed or finished: verify `iso` (then check
    // the toolchain). Ignored while work is running.
    void choose_image(const std::filesystem::path& iso);
    // toolchain or ready: check again.
    void check_toolchain();
    // ready or toolchain: start the setup.
    void start_setup();
    // running: ask the setup to stop at its next safe point.
    void cancel();
    // cancelled or failed: run the setup again; finished stages are skipped.
    void resume();
    // Back to choose_image, keeping the log. Ignored while work is running.
    void restart();

    void set_jobs(unsigned jobs) noexcept { jobs_ = jobs; } // 0: the backend's default
    void set_force(bool force) noexcept { force_ = force; }
    unsigned jobs() const noexcept { return jobs_; }
    bool force() const noexcept { return force_; }
    unsigned default_jobs();

    // Applies what the worker reported.
    void pump();

    WizardStep step() const noexcept { return step_; }
    bool busy() const noexcept;
    const std::filesystem::path& image() const noexcept { return image_; }
    const std::optional<VerifySummary>& verified() const noexcept { return verified_; }
    const std::optional<ToolchainReport>& toolchain() const noexcept { return toolchain_; }
    const std::optional<Problem>& problem() const noexcept { return problem_; }
    const std::optional<core::setup::Result>& result() const noexcept { return result_; }
    const SetupProgress& progress() const noexcept { return progress_; }
    const LogBuffer& log() const noexcept { return log_; }
    Clock::time_point now() const { return clock_(); }

  private:
    struct VerifyDone {
        std::optional<VerifySummary> summary;
        std::optional<Problem> problem;
    };
    struct ToolchainDone {
        std::optional<ToolchainReport> report;
        std::optional<Problem> problem;
    };
    struct SetupEvent {
        core::setup::Event event;
        Clock::time_point at;
    };
    struct SetupDone {
        std::optional<core::setup::Result> result;
        std::optional<Problem> problem;
    };
    using Message = std::variant<VerifyDone, ToolchainDone, SetupEvent, SetupDone>;

    void run_setup();
    void apply(VerifyDone& m);
    void apply(ToolchainDone& m);
    void apply(SetupEvent& m);
    void apply(SetupDone& m);

    WizardBackend& backend_;
    ExecutorFactory executors_;
    std::unique_ptr<Executor> executor_;
    std::filesystem::path executable_dir_;
    std::filesystem::path game_executable_;
    std::function<Clock::time_point()> clock_;
    std::shared_ptr<Mailbox<Message>> mailbox_ = std::make_shared<Mailbox<Message>>();
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);

    WizardStep step_ = WizardStep::choose_image;
    std::filesystem::path image_;
    std::optional<VerifySummary> verified_;
    std::optional<ToolchainReport> toolchain_;
    std::optional<Problem> problem_;
    std::optional<core::setup::Result> result_;
    SetupProgress progress_;
    LogBuffer log_{6000};
    unsigned jobs_ = 0;
    bool force_ = false;
    std::optional<unsigned> default_jobs_;
};

} // namespace dq8::launcher
