// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "model/wizard.h"

#include <utility>

namespace dq8::launcher {

Wizard::Wizard(WizardBackend& backend, ExecutorFactory executors, std::filesystem::path executable_dir,
               std::function<Clock::time_point()> clock)
    : backend_(backend), executors_(std::move(executors)), executable_dir_(std::move(executable_dir)),
      clock_(clock ? std::move(clock) : [] { return Clock::now(); }) {}

Wizard::~Wizard() {
    cancel_->store(true);
    executor_.reset(); // waits for a running job before the members it reports to go away
}

bool Wizard::busy() const noexcept {
    return step_ == WizardStep::verifying || step_ == WizardStep::toolchain_checking ||
           step_ == WizardStep::running || step_ == WizardStep::cancelling;
}

unsigned Wizard::default_jobs() {
    if (!default_jobs_) {
        try {
            default_jobs_ = backend_.default_jobs();
        } catch (...) {
            default_jobs_ = 1;
        }
    }
    return *default_jobs_;
}

void Wizard::choose_image(const std::filesystem::path& iso) {
    if (busy() || iso.empty()) {
        return;
    }
    if (!executor_) {
        executor_ = executors_();
    }
    image_ = iso;
    verified_.reset();
    toolchain_.reset();
    problem_.reset();
    result_.reset();
    progress_.reset();
    step_ = WizardStep::verifying;
    log_.add("Verifying " + iso.string());
    executor_->run([&backend = backend_, mailbox = mailbox_, iso] {
        VerifyDone done;
        try {
            done.summary = backend.verify(iso);
        } catch (...) {
            done.problem = explain_current_exception();
        }
        mailbox->post(std::move(done));
    });
}

void Wizard::check_toolchain() {
    if (step_ != WizardStep::toolchain && step_ != WizardStep::ready && step_ != WizardStep::verifying) {
        return;
    }
    if (!executor_) {
        executor_ = executors_();
    }
    step_ = WizardStep::toolchain_checking;
    problem_.reset();
    log_.add("Checking the build tools");
    executor_->run([&backend = backend_, mailbox = mailbox_] {
        ToolchainDone done;
        try {
            done.report = backend.check_toolchain();
        } catch (...) {
            done.problem = explain_current_exception();
        }
        mailbox->post(std::move(done));
    });
}

void Wizard::start_setup() {
    if (step_ != WizardStep::ready && step_ != WizardStep::toolchain) {
        return;
    }
    progress_.reset();
    run_setup();
}

void Wizard::resume() {
    if (step_ != WizardStep::cancelled && step_ != WizardStep::failed) {
        return;
    }
    log_.add("Resuming the setup; finished stages are skipped");
    progress_.reset();
    run_setup();
}

void Wizard::run_setup() {
    if (!executor_) {
        executor_ = executors_();
    }
    step_ = WizardStep::running;
    problem_.reset();
    result_.reset();
    cancel_->store(false);

    core::setup::Options options;
    options.iso = image_;
    options.executable_dir = executable_dir_;
    options.game_executable = game_executable_; // empty: found beside executable_dir
    options.jobs = jobs_;
    options.force = force_;
    options.allow_toolchain_download = allow_download_;
    options.cancel = cancel_.get();
    log_.add("Starting the setup with " +
             (jobs_ == 0 ? std::string("the default number of") : std::to_string(jobs_)) + " parallel jobs" +
             (force_ ? ", rebuilding everything" : ""));
    executor_->run([&backend = backend_, mailbox = mailbox_, cancel = cancel_, clock = clock_,
                    options = std::move(options)]() mutable {
        options.on_event = [&mailbox, &clock](const core::setup::Event& event) {
            mailbox->post(SetupEvent{event, clock()});
        };
        SetupDone done;
        try {
            done.result = backend.run_setup(options);
        } catch (...) {
            done.problem = explain_current_exception();
        }
        mailbox->post(std::move(done));
        (void)cancel; // keeps options.cancel alive until the setup returned
    });
}

void Wizard::cancel() {
    if (step_ != WizardStep::running) {
        return;
    }
    cancel_->store(true);
    step_ = WizardStep::cancelling;
    log_.add("Cancelling: the setup stops at its next safe point");
}

void Wizard::restart() {
    if (busy()) {
        return;
    }
    step_ = WizardStep::choose_image;
    verified_.reset();
    toolchain_.reset();
    problem_.reset();
    result_.reset();
    progress_.reset();
}

void Wizard::pump() {
    for (Message& message : mailbox_->take()) {
        std::visit([this](auto& m) { apply(m); }, message);
    }
}

void Wizard::apply(VerifyDone& m) {
    if (step_ != WizardStep::verifying) {
        return;
    }
    if (m.problem || !m.summary) {
        problem_ = m.problem ? std::move(m.problem)
                             : std::optional<Problem>(explain(core::ErrorCode::unknown_image));
        step_ = WizardStep::verify_failed;
        log_.add("Verification failed: " + problem_->message +
                 (problem_->detail.empty() ? std::string() : " (" + problem_->detail + ")"));
        return;
    }
    verified_ = std::move(m.summary);
    log_.add("Verified: " + verified_->profile_id +
             (verified_->title.empty() ? "" : ", " + verified_->title));
    for (const std::string& w : verified_->warnings) {
        log_.add("  note: " + w);
    }
    check_toolchain();
}

void Wizard::apply(ToolchainDone& m) {
    if (step_ != WizardStep::toolchain_checking) {
        return;
    }
    if (m.problem || !m.report) {
        problem_ = m.problem;
        toolchain_.reset();
        step_ = WizardStep::toolchain;
        log_.add("The toolchain check failed: " + (problem_ ? problem_->message : std::string("no report")));
        return;
    }
    toolchain_ = std::move(m.report);
    for (const core::toolchain::Problem& p : toolchain_->problems) {
        log_.add("  missing " + p.tool + ": " + p.advice);
    }
    step_ = toolchain_->problems.empty() ? WizardStep::ready : WizardStep::toolchain;
    log_.add(toolchain_->problems.empty() ? "The build tools are ready" : "Some build tools are missing");
}

void Wizard::apply(SetupEvent& m) {
    if (step_ != WizardStep::running && step_ != WizardStep::cancelling) {
        return;
    }
    progress_.apply(m.event, m.at);
    if (m.event.state != core::setup::State::progress || !m.event.message.empty()) {
        log_.add(format_event(m.event));
    }
}

void Wizard::apply(SetupDone& m) {
    if (step_ != WizardStep::running && step_ != WizardStep::cancelling) {
        return;
    }
    if (m.problem) {
        problem_ = std::move(m.problem);
        step_ = WizardStep::failed;
        log_.add("The setup could not run: " + problem_->message);
        return;
    }
    result_ = std::move(m.result);
    if (result_ && result_->ok) {
        step_ = WizardStep::finished;
        log_.add("The setup finished: " + result_->profile_id + " is ready to play");
        return;
    }
    const bool cancelled = result_ && result_->code == core::ErrorCode::setup_cancelled;
    if (result_ && result_->code) {
        problem_ = explain(*result_->code, result_->message);
    } else {
        Problem p;
        p.message =
            result_ && !result_->message.empty() ? result_->message : "The setup ended without a result.";
        problem_ = std::move(p);
    }
    step_ = cancelled ? WizardStep::cancelled : WizardStep::failed;
    log_.add(cancelled ? "The setup was cancelled; resume continues where it stopped"
                       : "The setup failed: " + problem_->message);
}

} // namespace dq8::launcher
