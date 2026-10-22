// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The launcher's reading of dq8-game's status channel (Docs/milestones/M5-launcher.md "The status channel").
#include "model/status.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using namespace dq8::launcher;

TEST_CASE("launcher: the five status lines parse", "[launcher][status]") {
    const auto started = parse_status_line(
        R"({"ev":"started","version":"0.1.0","profile":"SLUS_21207","pid":1234,"window":true})");
    REQUIRE(started);
    const auto& s = std::get<StatusStarted>(*started);
    CHECK(s.version == "0.1.0");
    CHECK(s.profile == "SLUS_21207");
    CHECK(s.pid == 1234);
    CHECK(s.window);

    const auto first = parse_status_line(R"({"ev":"first_frame","ms":812})");
    REQUIRE(first);
    CHECK(std::get<StatusFirstFrame>(*first).ms == 812.0);

    const auto fps = parse_status_line(R"({"ev":"fps","fps":59.94,"speed":0.5})");
    REQUIRE(fps);
    CHECK(std::get<StatusFps>(*fps).fps == 59.94);
    CHECK(std::get<StatusFps>(*fps).speed == 0.5);

    const auto warning = parse_status_line(R"({"ev":"warning","code":"maps-missing","message":"no map"})");
    REQUIRE(warning);
    CHECK(std::get<StatusWarning>(*warning).code == "maps-missing");
    CHECK(std::get<StatusWarning>(*warning).message == "no map");

    const auto exit_line =
        parse_status_line(R"({"ev":"exit","status":2,"reason":"error","code":"smoke-failed","message":"x"})");
    REQUIRE(exit_line);
    const auto& e = std::get<StatusExit>(*exit_line);
    REQUIRE(e.status);
    CHECK(*e.status == 2);
    CHECK(e.reason == "error");
    CHECK(e.code == "smoke-failed");
    CHECK(e.message == "x");
}

TEST_CASE("launcher: lines that are not status objects are output", "[launcher][status]") {
    CHECK_FALSE(parse_status_line(""));
    CHECK_FALSE(parse_status_line("dq8-game: loading modules"));
    CHECK_FALSE(parse_status_line("{not json"));
    CHECK_FALSE(parse_status_line(R"({"event":"started"})")); // no "ev"
    CHECK_FALSE(parse_status_line(R"({"ev":3})"));            // "ev" not a string
    CHECK_FALSE(parse_status_line(R"(["ev","started"])"));
}

TEST_CASE("launcher: unknown status events and odd fields are tolerated", "[launcher][status]") {
    const auto other = parse_status_line(R"({"ev":"module_loaded","unit":"main"})");
    REQUIRE(other);
    CHECK(std::get<StatusOther>(*other).ev == "module_loaded");

    // Wrong types leave the defaults; a trailing \r (Windows line ends) is ignored.
    const auto started = parse_status_line("{\"ev\":\"started\",\"pid\":\"x\",\"window\":1}\r");
    REQUIRE(started);
    CHECK(std::get<StatusStarted>(*started).pid == 0);
    CHECK_FALSE(std::get<StatusStarted>(*started).window);

    const auto exit_line = parse_status_line(R"({"ev":"exit","reason":"window-closed"})");
    REQUIRE(exit_line);
    CHECK_FALSE(std::get<StatusExit>(*exit_line).status);
}

TEST_CASE("launcher: the line splitter rebuilds lines from chunks", "[launcher][status]") {
    std::vector<std::string> lines;
    const auto collect = [&](std::string_view l) { lines.emplace_back(l); };
    LineSplitter split;
    split.feed("one\ntw", collect);
    split.feed("o\n\nthr", collect);
    split.feed("ee", collect);
    CHECK(lines == std::vector<std::string>{"one", "two", ""});
    split.finish(collect);
    CHECK(lines == std::vector<std::string>{"one", "two", "", "three"});
    split.finish(collect); // nothing pending
    CHECK(lines.size() == 4);
}

TEST_CASE("launcher: an overlong line is passed on in pieces", "[launcher][status]") {
    std::vector<std::string> lines;
    const auto collect = [&](std::string_view l) { lines.emplace_back(l); };
    LineSplitter split(4);
    split.feed("abcdefghij\nxy", collect);
    split.feed("z\n", collect);
    CHECK(lines == std::vector<std::string>{"abcd", "efgh", "ij", "xyz"});
}
