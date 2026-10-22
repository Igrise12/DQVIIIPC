// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Play: the command line, the exit summary and the supervisor, with a fake runner and with a real child
// process.
#include "model/game_session.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace dq8::launcher;
using dq8::core::ErrorCode;
using dq8::core::OutputStream;
using dq8::core::ProcessOptions;
using dq8::core::ProcessResult;

namespace {

PlayRequest request(const std::string& iso = "disc.iso") {
    PlayRequest r;
    r.game = "dq8-game";
    r.iso = iso;
    r.module_dir = "modules";
    r.config_file = "settings.toml";
    r.profile_id = "SLUS_21207";
    return r;
}

// Waits on a real worker thread until the session leaves its active phases.
void wait_for_exit(GameSupervisor& supervisor) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (supervisor.active() && std::chrono::steady_clock::now() < deadline) {
        supervisor.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    supervisor.pump();
}

} // namespace

TEST_CASE("launcher: dq8-game's arguments", "[launcher][play]") {
    CHECK(game_arguments(request()) == std::vector<std::string>{"--iso", "disc.iso", "--module-dir",
                                                                "modules", "--window", "--config",
                                                                "settings.toml", "--status", "-"});
    PlayRequest no_config = request();
    no_config.config_file.clear();
    CHECK(game_arguments(no_config) == std::vector<std::string>{"--iso", "disc.iso", "--module-dir",
                                                                "modules", "--window", "--status", "-"});
}

TEST_CASE("launcher: exit summaries", "[launcher][play]") {
    ProcessResult ok;
    ok.exit_code = 0;
    StatusExit closed;
    closed.status = 0;
    closed.reason = "window-closed";
    const ExitSummary a = summarize_exit(closed, ok, false);
    CHECK(a.ok);
    CHECK(a.reason == "window-closed");
    CHECK(a.exit_code == 0);

    // An error line: error_text() for a known code, the game's message kept as the detail.
    StatusExit error;
    error.status = 2;
    error.reason = "error";
    error.code = "smoke-failed";
    error.message = "the title never appeared";
    ProcessResult two;
    two.exit_code = 2;
    const ExitSummary b = summarize_exit(error, two, false);
    CHECK_FALSE(b.ok);
    REQUIRE(b.problem.code);
    CHECK(*b.problem.code == ErrorCode::smoke_failed);
    CHECK_FALSE(b.problem.action.empty());
    CHECK(b.problem.detail == "the title never appeared");

    // An unknown code keeps the game's own message.
    error.code = "gs-device-lost";
    const ExitSummary c = summarize_exit(error, two, false);
    CHECK_FALSE(c.problem.code);
    CHECK(c.problem.code_name == "gs-device-lost");
    CHECK(c.problem.message == "the title never appeared");

    // No status line: the exit status decides.
    ProcessResult three;
    three.exit_code = 3;
    const ExitSummary d = summarize_exit(std::nullopt, three, false);
    CHECK_FALSE(d.ok);
    CHECK(d.problem.message.find("graphics") != std::string::npos);

    ProcessResult crashed;
    crashed.exit_code = -1;
    crashed.signal = 11;
    const ExitSummary e = summarize_exit(std::nullopt, crashed, false);
    CHECK_FALSE(e.ok);
    CHECK(e.signal == 11);
    CHECK_FALSE(e.exit_code);
    CHECK(e.problem.message.find("signal 11") != std::string::npos);

    ProcessResult stopped;
    stopped.cancelled = true;
    const ExitSummary f = summarize_exit(std::nullopt, stopped, true);
    CHECK(f.ok);
    CHECK(f.stopped);
}

TEST_CASE("launcher: the supervisor feeds the session from the runner", "[launcher][play]") {
    std::vector<std::string> seen_args;
    GameSupervisor supervisor(inline_executors(), [&](const ProcessOptions& options) {
        seen_args = options.args;
        REQUIRE(options.cancel != nullptr);
        // Chunks that split lines and mix status lines with output.
        options.on_output(OutputStream::out,
                          "{\"ev\":\"started\",\"version\":\"0.0.1\",\"profile\":\"SLUS_21207\",");
        options.on_output(OutputStream::out,
                          "\"pid\":7,\"window\":true}\nloading\n{\"ev\":\"first_frame\",\"ms\":90}\n");
        options.on_output(OutputStream::err, "a warning on stderr\n");
        options.on_output(OutputStream::out, "{\"ev\":\"fps\",\"fps\":60,\"speed\":1}\n");
        options.on_output(OutputStream::out, "{\"ev\":\"warning\",\"code\":\"x\",\"message\":\"y\"}\n");
        options.on_output(OutputStream::out, "{\"ev\":\"exit\",\"status\":0,\"reason\":\"window-closed\"}");
        ProcessResult r;
        r.exit_code = 0;
        return r;
    });
    CHECK(supervisor.session().phase() == GamePhase::idle);
    REQUIRE(supervisor.start(request()));
    CHECK(seen_args == game_arguments(request()));
    supervisor.pump();
    const GameSession& s = supervisor.session();
    CHECK(s.phase() == GamePhase::exited);
    REQUIRE(s.started());
    CHECK(s.started()->pid == 7);
    REQUIRE(s.first_frame_ms());
    CHECK(*s.first_frame_ms() == 90.0);
    REQUIRE(s.fps());
    CHECK(s.fps()->fps == 60.0);
    REQUIRE(s.warnings().size() == 1);
    REQUIRE(s.exit_line()); // the unterminated last line still counts
    REQUIRE(s.exit());
    CHECK(s.exit()->ok);
    // Output and warnings reach the log; status lines do not.
    const auto& lines = s.log().lines();
    CHECK(std::find(lines.begin(), lines.end(), "loading") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "a warning on stderr") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "warning: x: y") != lines.end());
    CHECK(lines.size() == 3);
}

TEST_CASE("launcher: a game that cannot start reports why", "[launcher][play]") {
    GameSupervisor supervisor(inline_executors(), [](const ProcessOptions&) -> ProcessResult {
        throw dq8::core::Error(ErrorCode::process_spawn, "cannot start dq8-game: not found");
    });
    REQUIRE(supervisor.start(request()));
    supervisor.pump();
    REQUIRE(supervisor.session().exit());
    const ExitSummary& e = *supervisor.session().exit();
    CHECK_FALSE(e.ok);
    REQUIRE(e.problem.code);
    CHECK(*e.problem.code == ErrorCode::process_spawn);
    CHECK(e.problem.detail == "cannot start dq8-game: not found");
    // A new start is allowed once the last one ended.
    CHECK_FALSE(supervisor.active());
    CHECK(supervisor.start(request()));
}

#ifdef DQ8_LAUNCHER_FAKE_GAME
TEST_CASE("launcher: the supervisor runs a real child process", "[launcher][play]") {
    GameSupervisor supervisor(thread_executors());
    PlayRequest r = request("ok");
    r.game = DQ8_LAUNCHER_FAKE_GAME;
    REQUIRE(supervisor.start(r));
    CHECK_FALSE(supervisor.start(r)); // one game at a time
    wait_for_exit(supervisor);
    const GameSession& s = supervisor.session();
    REQUIRE(s.exit());
    CHECK(s.exit()->ok);
    CHECK(s.exit()->exit_code == 0);
    REQUIRE(s.started());
    CHECK(s.started()->profile == "FAKE");
    REQUIRE(s.fps());
    CHECK(s.fps()->speed == 0.99);
    REQUIRE(s.warnings().size() == 1);
    CHECK(s.warnings()[0].code == "audio-underrun");
    const auto& lines = s.log().lines();
    CHECK(std::find(lines.begin(), lines.end(), "arg: --status") != lines.end());
}

TEST_CASE("launcher: a real child's error exit and silent failure", "[launcher][play]") {
    GameSupervisor supervisor(thread_executors());
    PlayRequest r = request("error");
    r.game = DQ8_LAUNCHER_FAKE_GAME;
    REQUIRE(supervisor.start(r));
    wait_for_exit(supervisor);
    REQUIRE(supervisor.session().exit());
    CHECK_FALSE(supervisor.session().exit()->ok);
    REQUIRE(supervisor.session().exit()->problem.code);
    CHECK(*supervisor.session().exit()->problem.code == ErrorCode::smoke_failed);
    CHECK(supervisor.session().exit()->exit_code == 2);

    r.iso = "silent";
    REQUIRE(supervisor.start(r));
    wait_for_exit(supervisor);
    REQUIRE(supervisor.session().exit());
    CHECK(supervisor.session().exit()->exit_code == 3);
    CHECK_FALSE(supervisor.session().started());
    const auto& lines = supervisor.session().log().lines();
    CHECK(std::find(lines.begin(), lines.end(), "an ordinary line on stdout") != lines.end());
}

TEST_CASE("launcher: stopping a running game", "[launcher][play]") {
    GameSupervisor supervisor(thread_executors());
    PlayRequest r = request("hang");
    r.game = DQ8_LAUNCHER_FAKE_GAME;
    REQUIRE(supervisor.start(r));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (supervisor.session().phase() != GamePhase::running &&
           std::chrono::steady_clock::now() < deadline) {
        supervisor.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(supervisor.session().phase() == GamePhase::running);
    supervisor.stop();
    wait_for_exit(supervisor);
    REQUIRE(supervisor.session().exit());
    CHECK(supervisor.session().exit()->stopped);
    CHECK(supervisor.session().exit()->ok);
}
#endif
