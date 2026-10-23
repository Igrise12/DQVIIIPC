// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The first-run wizard: the per-stage progress model and the state machine over a fake backend.
#include "model/wizard.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace dq8::launcher;
using dq8::core::ErrorCode;
using dq8::core::setup::Event;
using dq8::core::setup::Stage;
using dq8::core::setup::State;
using namespace std::chrono_literals;

namespace {

Event ev(Stage stage, State state, std::uint64_t done = 0, std::uint64_t total = 0, std::string unit = {},
         std::string message = {}, std::optional<ErrorCode> code = {}) {
    return Event{stage, state, done, total, std::move(unit), std::move(message), code};
}

class FakeBackend final : public WizardBackend {
  public:
    std::optional<ErrorCode> verify_error;
    bool verify_throws_plain = false;
    std::vector<dq8::core::toolchain::Problem> problems;
    bool toolchain_throws = false;
    std::vector<Event> events;
    dq8::core::setup::Result result{true, std::nullopt, {}, "SLUS_21207", "modules", {}, {}};
    bool setup_throws = false;
    bool wait_for_cancel = false;
    int verifies = 0;
    int toolchain_checks = 0;
    int setups = 0;
    dq8::core::setup::Options last_options;

    VerifySummary verify(const std::filesystem::path& iso) override {
        ++verifies;
        if (verify_throws_plain) {
            throw std::logic_error("intake is not implemented");
        }
        if (verify_error) {
            throw dq8::core::Error(*verify_error, "boot file sha1 1234 matches no profile: " + iso.string());
        }
        VerifySummary s;
        s.profile_id = "SLUS_21207";
        s.title = "a title";
        s.warnings = {"a note"};
        return s;
    }
    ToolchainReport check_toolchain() override {
        ++toolchain_checks;
        if (toolchain_throws) {
            throw std::logic_error("toolchain: detect is not implemented (M5 session A)");
        }
        ToolchainReport r;
        r.problems = problems;
        return r;
    }
    unsigned default_jobs() override { return 6; }
    dq8::core::setup::Result run_setup(const dq8::core::setup::Options& options) override {
        ++setups;
        last_options = options;
        if (setup_throws) {
            throw std::logic_error("setup: run is not implemented (M5 session A)");
        }
        for (const Event& e : events) {
            options.on_event(e);
        }
        if (wait_for_cancel) {
            while (!options.cancel->load()) {
                std::this_thread::sleep_for(1ms);
            }
            options.on_event(
                ev(Stage::build, State::error, 3, 10, {}, "cancelled", ErrorCode::setup_cancelled));
            return {false, ErrorCode::setup_cancelled, "cancelled", {}, {}, {}, {}};
        }
        return result;
    }
};

// A clock the test moves by hand.
struct TestClock {
    std::chrono::steady_clock::time_point now{std::chrono::seconds(1000)};
};

} // namespace

TEST_CASE("launcher: stage titles and event log lines", "[launcher][wizard]") {
    for (std::size_t i = 0; i < stage_count; ++i) {
        CHECK(stage_index(stage_at(i)) == i);
        CHECK_FALSE(stage_title(stage_at(i)).empty());
    }
    CHECK(stage_at(0) == Stage::verify);
    CHECK(stage_at(stage_count - 1) == Stage::register_install);
    CHECK(format_event(ev(Stage::build, State::start)) == "[build] started");
    CHECK(format_event(ev(Stage::build, State::progress, 120, 845, "main", "cc a.cpp")) ==
          "[build] main: cc a.cpp (120/845)");
    CHECK(format_event(ev(Stage::extract, State::skipped)) == "[extract] up to date, skipped");
    CHECK(format_event(ev(Stage::smoke, State::error, 0, 0, {}, "no title", ErrorCode::smoke_failed)) ==
          "[smoke] failed (smoke-failed): no title");
    CHECK(format_event(ev(Stage::register_install, State::done)) == "[register] done");
}

TEST_CASE("launcher: setup progress follows the stage events", "[launcher][wizard]") {
    SetupProgress p;
    const auto t0 = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
    CHECK(p.overall() == 0.0);
    CHECK_FALSE(p.current());

    p.apply(ev(Stage::verify, State::start), t0);
    p.apply(ev(Stage::verify, State::done), t0 + 1s);
    p.apply(ev(Stage::extract, State::skipped), t0 + 1s);
    CHECK(p.stage(Stage::verify).state == StageState::done);
    CHECK(p.stage(Stage::extract).state == StageState::skipped);
    CHECK_FALSE(p.current());
    const double after_two = p.overall();
    CHECK(after_two > 0.0);

    p.apply(ev(Stage::build, State::start, 0, 800), t0 + 10s);
    REQUIRE(p.current());
    CHECK(*p.current() == Stage::build);
    p.apply(ev(Stage::build, State::progress, 200, 800, "main"), t0 + 110s);
    CHECK(p.stage(Stage::build).fraction() == 0.25);
    CHECK(p.stage(Stage::build).unit == "main");
    CHECK(p.overall() > after_two);
    // 200 steps in 100 s: 600 more take 300 s.
    REQUIRE(p.remaining(t0 + 110s));
    CHECK(p.remaining(t0 + 110s)->count() == 300);

    // Unknown totals give no fraction and no estimate.
    p.apply(ev(Stage::build, State::progress, 5, 0), t0 + 111s);
    CHECK_FALSE(p.stage(Stage::build).fraction());
    CHECK_FALSE(p.remaining(t0 + 112s));

    p.apply(ev(Stage::build, State::error, 5, 0, {}, "cc failed", ErrorCode::tool_missing), t0 + 112s);
    CHECK(p.stage(Stage::build).state == StageState::failed);
    REQUIRE(p.stage(Stage::build).code);
    CHECK(*p.stage(Stage::build).code == ErrorCode::tool_missing);
    CHECK(p.stage(Stage::build).message == "cc failed");

    // All stages done: complete.
    SetupProgress all;
    for (std::size_t i = 0; i < stage_count; ++i) {
        all.apply(ev(stage_at(i), State::done), t0);
    }
    CHECK(all.overall() == 1.0);
    all.reset();
    CHECK(all.overall() == 0.0);
}

TEST_CASE("launcher: the wizard from the image to a finished setup", "[launcher][wizard]") {
    FakeBackend backend;
    backend.events = {
        ev(Stage::verify, State::start),         ev(Stage::verify, State::done),
        ev(Stage::build, State::start, 0, 4),    ev(Stage::build, State::progress, 2, 4, "main", "cc"),
        ev(Stage::build, State::progress, 3, 4), ev(Stage::build, State::done, 4, 4),
        ev(Stage::register_install, State::done)};
    TestClock clock;
    Wizard w(backend, inline_executors(), "/opt/dq8/bin", [&] { return clock.now; });
    CHECK(w.step() == WizardStep::choose_image);
    CHECK(w.default_jobs() == 6);

    w.choose_image("/discs/dq8.iso");
    CHECK(w.step() == WizardStep::verifying);
    CHECK(w.busy());
    w.pump(); // verified, toolchain check started
    REQUIRE(w.verified());
    CHECK(w.verified()->profile_id == "SLUS_21207");
    CHECK(w.step() == WizardStep::toolchain_checking);
    w.pump();
    CHECK(w.step() == WizardStep::ready);
    REQUIRE(w.toolchain());
    CHECK(w.toolchain()->problems.empty());

    w.set_jobs(3);
    w.start_setup();
    CHECK(w.step() == WizardStep::running);
    CHECK(backend.last_options.iso == "/discs/dq8.iso");
    CHECK(backend.last_options.executable_dir == "/opt/dq8/bin");
    CHECK(backend.last_options.jobs == 3);
    CHECK_FALSE(backend.last_options.force);
    CHECK(backend.last_options.cancel != nullptr);
    CHECK_FALSE(backend.last_options.allow_toolchain_download); // no consent unless the user gives it
    CHECK_FALSE(w.bundle_offer().available);                    // the fake offers no download
    w.pump();
    CHECK(w.step() == WizardStep::finished);
    CHECK_FALSE(w.busy());
    CHECK(w.progress().stage(Stage::build).state == StageState::done);
    REQUIRE(w.result());
    CHECK(w.result()->ok);
    // The log has the verification, the note, the stages, and the progress line that carried a message.
    const auto& lines = w.log().lines();
    CHECK(std::find(lines.begin(), lines.end(), "  note: a note") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "[build] main: cc (2/4)") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "[build] done") != lines.end());
}

TEST_CASE("launcher: a disc that does not verify", "[launcher][wizard]") {
    FakeBackend backend;
    backend.verify_error = ErrorCode::unknown_image;
    Wizard w(backend, inline_executors(), {});
    w.choose_image("other.iso");
    w.pump();
    CHECK(w.step() == WizardStep::verify_failed);
    REQUIRE(w.problem());
    REQUIRE(w.problem()->code);
    CHECK(*w.problem()->code == ErrorCode::unknown_image);
    CHECK_FALSE(w.problem()->action.empty());
    CHECK(w.problem()->detail.find("other.iso") != std::string::npos);
    CHECK(backend.toolchain_checks == 0);

    // Another image from the failed step.
    backend.verify_error.reset();
    w.choose_image("dq8.iso");
    w.pump();
    w.pump();
    CHECK(w.step() == WizardStep::ready);
    CHECK_FALSE(w.problem());
}

TEST_CASE("launcher: toolchain problems, check again, continue anyway", "[launcher][wizard]") {
    FakeBackend backend;
    backend.problems = {{ErrorCode::toolchain_incomplete, "ninja", "Install ninja-build"}};
    Wizard w(backend, inline_executors(), {});
    w.choose_image("dq8.iso");
    w.pump();
    w.pump();
    CHECK(w.step() == WizardStep::toolchain);
    REQUIRE(w.toolchain());
    REQUIRE(w.toolchain()->problems.size() == 1);
    CHECK(w.toolchain()->problems[0].tool == "ninja");

    backend.problems.clear();
    w.check_toolchain();
    CHECK(w.step() == WizardStep::toolchain_checking);
    w.pump();
    CHECK(w.step() == WizardStep::ready);
    CHECK(backend.toolchain_checks == 2);

    // A check that cannot run at all (the day-0 stub) leaves the step with its problem; the setup may still
    // start.
    backend.toolchain_throws = true;
    w.check_toolchain();
    w.pump();
    CHECK(w.step() == WizardStep::toolchain);
    REQUIRE(w.problem());
    CHECK_FALSE(w.problem()->code);
    CHECK(w.problem()->message.find("not implemented") != std::string::npos);
    w.start_setup();
    CHECK(w.step() == WizardStep::running);
    w.pump();
    CHECK(w.step() == WizardStep::finished);
}

TEST_CASE("launcher: setup failures and the stubbed setup", "[launcher][wizard]") {
    FakeBackend backend;
    backend.result = {false, ErrorCode::module_build_failed, "ninja: build stopped", {}, {}, {}, {}};
    backend.events = {
        ev(Stage::build, State::start, 0, 10),
        ev(Stage::build, State::error, 2, 10, {}, "build stopped", ErrorCode::module_build_failed)};
    Wizard w(backend, inline_executors(), {});
    w.choose_image("dq8.iso");
    w.pump();
    w.pump();
    w.start_setup();
    w.pump();
    CHECK(w.step() == WizardStep::failed);
    REQUIRE(w.problem());
    CHECK(*w.problem()->code == ErrorCode::module_build_failed);
    CHECK(w.problem()->detail == "ninja: build stopped");
    CHECK(w.progress().stage(Stage::build).state == StageState::failed);

    // Resume retries; this time it works, and the stale failure is gone.
    backend.result = {true, std::nullopt, {}, "SLUS_21207", "modules", {}, {}};
    backend.events.clear();
    w.set_force(true);
    w.resume();
    CHECK(backend.last_options.force);
    w.pump();
    CHECK(w.step() == WizardStep::finished);
    CHECK_FALSE(w.problem());
    CHECK(w.progress().stage(Stage::build).state == StageState::pending);

    // The day-0 stub throws std::logic_error: shown as a failure without a code.
    FakeBackend stub;
    stub.setup_throws = true;
    Wizard s(stub, inline_executors(), {});
    s.choose_image("dq8.iso");
    s.pump();
    s.pump();
    s.start_setup();
    s.pump();
    CHECK(s.step() == WizardStep::failed);
    REQUIRE(s.problem());
    CHECK_FALSE(s.problem()->code);
    CHECK(s.problem()->message.find("not implemented") != std::string::npos);
}

TEST_CASE("launcher: cancel and resume on a worker thread", "[launcher][wizard]") {
    FakeBackend backend;
    backend.wait_for_cancel = true;
    backend.events = {ev(Stage::build, State::start, 0, 10), ev(Stage::build, State::progress, 3, 10)};
    Wizard w(backend, thread_executors(), {});
    const auto settle = [&](WizardStep until) {
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (w.step() != until && std::chrono::steady_clock::now() < deadline) {
            w.pump();
            std::this_thread::sleep_for(1ms);
        }
        return w.step();
    };
    w.choose_image("dq8.iso");
    REQUIRE(settle(WizardStep::ready) == WizardStep::ready);
    w.start_setup();
    // Wait until the progress shows the running build, then cancel.
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (w.progress().stage(Stage::build).done != 3 && std::chrono::steady_clock::now() < deadline) {
        w.pump();
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(w.progress().stage(Stage::build).done == 3);
    w.cancel();
    CHECK(w.step() == WizardStep::cancelling);
    REQUIRE(settle(WizardStep::cancelled) == WizardStep::cancelled);
    REQUIRE(w.problem());
    CHECK(*w.problem()->code == ErrorCode::setup_cancelled);

    // Resume: the cancel flag is cleared for the new run.
    backend.wait_for_cancel = false;
    w.resume();
    REQUIRE(settle(WizardStep::finished) == WizardStep::finished);
    CHECK(backend.setups == 2);
}

TEST_CASE("launcher: wizard calls that do not fit the step are ignored", "[launcher][wizard]") {
    FakeBackend backend;
    Wizard w(backend, inline_executors(), {});
    w.start_setup(); // no image yet
    w.cancel();
    w.resume();
    w.check_toolchain();
    CHECK(w.step() == WizardStep::choose_image);
    CHECK(backend.setups == 0);
    CHECK(backend.toolchain_checks == 0);
    w.choose_image(""); // no path
    CHECK(w.step() == WizardStep::choose_image);
    w.choose_image("dq8.iso");
    w.pump();
    w.pump();
    w.restart();
    CHECK(w.step() == WizardStep::choose_image);
    CHECK_FALSE(w.verified());
}

TEST_CASE("launcher: the toolchain download needs the user's consent", "[launcher][wizard]") {
    // A backend that offers the bundled toolchain, as on Windows.
    class OfferingBackend final : public WizardBackend {
      public:
        VerifySummary verify(const std::filesystem::path&) override {
            return {"SLUS_21207", "t", "s", 1, {}};
        }
        ToolchainReport check_toolchain() override {
            ToolchainReport r;
            r.problems.push_back({ErrorCode::toolchain_incomplete, "c++", "let the setup download it"});
            return r;
        }
        unsigned default_jobs() override { return 2; }
        dq8::core::setup::Result run_setup(const dq8::core::setup::Options& options) override {
            consent = options.allow_toolchain_download;
            return {false, ErrorCode::toolchain_incomplete, "allow the download", {}, {}, {}, {}};
        }
        dq8::core::toolchain::BundleOffer bundle_offer() override { return {true, "20261020", 190u << 20}; }
        bool consent = true;
    };
    OfferingBackend backend;
    Wizard w(backend, inline_executors(), "/opt/dq8/bin");
    w.choose_image("/discs/dq8.iso");
    w.pump();
    w.pump();
    CHECK(w.step() == WizardStep::toolchain);
    REQUIRE(w.bundle_offer().available);
    CHECK(w.bundle_offer().version == "20261020");
    w.start_setup(); // without consent: the setup stops at the build tools
    w.pump();
    CHECK_FALSE(backend.consent);
    CHECK(w.step() == WizardStep::failed);
    REQUIRE(w.problem());
    CHECK(w.problem()->code == ErrorCode::toolchain_incomplete);
    w.set_allow_download(true); // the checkbox, then "Try again"
    w.resume();
    w.pump();
    CHECK(backend.consent);
}
