// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/error.h"
#include "dq8/core/process.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using dq8::core::Error;
using dq8::core::ErrorCode;
using dq8::core::OutputStream;
using dq8::core::ProcessOptions;
using dq8::core::ProcessResult;
using dq8::core::run_process;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

ProcessOptions child(std::vector<std::string> args) {
    ProcessOptions o;
    o.program = DQ8_TEST_CHILD;
    o.args = std::move(args);
    o.timeout = 60s; // a hung test fails instead of blocking the suite
    return o;
}

// Windows text-mode stdout writes CRLF.
std::string lf(std::string s) {
    std::erase(s, '\r');
    return s;
}

} // namespace

TEST_CASE("run_process captures stdout and stderr separately and the exit status", "[core][process]") {
    const ProcessResult r = run_process(child({"streams"}));
    CHECK(r.ok());
    CHECK(r.exit_code == 0);
    CHECK(lf(r.out) == "to-stdout\n");
    CHECK(lf(r.err) == "to-stderr\n");
    CHECK_FALSE(r.timed_out);
    CHECK_FALSE(r.cancelled);

    const ProcessResult failed = run_process(child({"exit", "7"}));
    CHECK_FALSE(failed.ok());
    CHECK(failed.exit_code == 7);
}

TEST_CASE("run_process passes arguments verbatim, without a shell", "[core][process]") {
    const std::vector<std::string> args{
        "plain", "with space", "",           "quote\"inside", "trailing\\", "back\\\\slash\"q",
        "$HOME", "*.cpp",      "semi;colon", "a\tb"};
    std::vector<std::string> argv{"echo-args"};
    argv.insert(argv.end(), args.begin(), args.end());
    const ProcessResult r = run_process(child(argv));
    REQUIRE(r.ok());
    std::string expected;
    for (const std::string& a : args) {
        expected += "[" + a + "]\n";
    }
    CHECK(lf(r.out) == expected);
}

TEST_CASE("run_process honours the working directory and environment overrides", "[core][process]") {
    const fs::path dir = fs::temp_directory_path();
    ProcessOptions o = child({"cwd"});
    o.working_dir = dir;
    const ProcessResult r = run_process(o);
    REQUIRE(r.ok());
    CHECK(fs::equivalent(fs::path(lf(r.out).substr(0, lf(r.out).size() - 1)), dir));

    ProcessOptions e = child({"env", "DQ8_PROCESS_TEST_VAR"});
    e.env.emplace_back("DQ8_PROCESS_TEST_VAR", "value with spaces");
    CHECK(lf(run_process(e).out) == "value with spaces\n");
    CHECK(lf(run_process(child({"env", "DQ8_PROCESS_TEST_VAR"})).out) == "<unset>\n");
}

TEST_CASE("run_process drains large output on both pipes without deadlock", "[core][process]") {
    std::size_t streamed_out = 0;
    std::size_t streamed_err = 0;
    ProcessOptions o = child({"flood", "1048576"});
    o.on_output = [&](OutputStream s, std::string_view chunk) {
        (s == OutputStream::out ? streamed_out : streamed_err) += chunk.size();
    };
    o.capture_limit = 100000;
    const ProcessResult r = run_process(o);
    REQUIRE(r.ok());
    CHECK(streamed_out == 1048576);
    CHECK(streamed_err == 1048576);
    CHECK(r.out.size() == 100000);
    CHECK(r.out_truncated);
    CHECK(r.err_truncated);
}

TEST_CASE("run_process stops a child that outlives its timeout", "[core][process]") {
    ProcessOptions o = child({"sleep", "30000"});
    o.timeout = 200ms;
    const auto start = std::chrono::steady_clock::now();
    const ProcessResult r = run_process(o);
    CHECK(r.timed_out);
    CHECK_FALSE(r.ok());
    CHECK(r.exit_code == -1);
    CHECK(std::chrono::steady_clock::now() - start < 10s);
}

TEST_CASE("run_process stops a child when the caller cancels", "[core][process]") {
    std::atomic<bool> cancel{false};
    ProcessOptions o = child({"sleep", "30000"});
    o.cancel = &cancel;
    std::thread canceller([&] {
        std::this_thread::sleep_for(150ms);
        cancel = true;
    });
    const ProcessResult r = run_process(o);
    canceller.join();
    CHECK(r.cancelled);
    CHECK_FALSE(r.ok());
    CHECK(r.elapsed < 10s);
}

TEST_CASE("run_process reports a program that cannot be started", "[core][process]") {
    ProcessOptions o;
    o.program = "dq8-no-such-program-anywhere";
    try {
        (void)run_process(o);
        FAIL("expected process_spawn");
    } catch (const Error& e) {
        CHECK(e.code() == ErrorCode::process_spawn);
        CHECK_THAT(e.what(), ContainsSubstring("dq8-no-such-program-anywhere"));
    }
    ProcessOptions bad_dir = child({"cwd"});
    bad_dir.working_dir = fs::temp_directory_path() / "dq8-no-such-directory";
    CHECK_THROWS_AS(run_process(bad_dir), Error);
}

TEST_CASE("find_program resolves paths and PATH lookups", "[core][process]") {
    const auto direct = dq8::core::find_program(DQ8_TEST_CHILD);
    REQUIRE(direct.has_value());
    CHECK(direct->is_absolute());
    CHECK_FALSE(dq8::core::find_program("dq8-no-such-program-anywhere").has_value());
    CHECK_FALSE(dq8::core::find_program(fs::temp_directory_path()).has_value()); // a directory
}

TEST_CASE("Windows argument quoting round-trips the CommandLineToArgvW rules", "[core][process]") {
    using dq8::core::quote_windows_argument;
    CHECK(quote_windows_argument("plain") == "plain");
    CHECK(quote_windows_argument("") == "\"\"");
    CHECK(quote_windows_argument("a b") == "\"a b\"");
    CHECK(quote_windows_argument("a\"b") == "\"a\\\"b\"");
    CHECK(quote_windows_argument("C:\\dir\\") == "C:\\dir\\");
    CHECK(quote_windows_argument("C:\\my dir\\") == "\"C:\\my dir\\\\\"");
    CHECK(quote_windows_argument("x\\\"y") == "\"x\\\\\\\"y\"");
}

TEST_CASE("describe_command quotes what a shell would split", "[core][process]") {
    CHECK(dq8::core::describe_command("cmake", {"-S", "a b", "it's", "--build"}) ==
          "cmake -S 'a b' 'it'\\''s' --build");
}
