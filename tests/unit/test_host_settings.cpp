// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// M5 session A: dq8-game's settings-to-options step (host/settings_wiring.h) and the status channel
// (host/status_channel.h). The end-to-end runs are in m5_core_tests.cmake.
#include "dq8/core/error.h"
#include "host/options.h"
#include "host/settings_wiring.h"
#include "host/status_channel.h"
#include "support/iso_builder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

dq8::host::Options parse(std::initializer_list<const char*> args) {
    std::vector<const char*> argv{"dq8-game"};
    argv.insert(argv.end(), args.begin(), args.end());
    return dq8::host::parse_options(static_cast<int>(argv.size()), argv.data());
}

const fs::path exe_dir =
    fs::path(DQ8_CONTRACT_MODULE_DIR); // inside the build tree: the schema is found from it

std::vector<std::string> lines_of(const fs::path& file) {
    std::ifstream in(file);
    std::vector<std::string> out;
    for (std::string line; std::getline(in, line);) {
        out.push_back(line);
    }
    return out;
}

} // namespace

TEST_CASE("dq8-game options remember which flags were given", "[m5][host]") {
    const dq8::host::Options o = parse({"--elf", "a.elf", "--module-dir", "m", "--window", "--movies=skip"});
    CHECK(o.was_given("elf"));
    CHECK(o.was_given("window"));
    CHECK(o.was_given("movies"));
    CHECK_FALSE(o.was_given("audio"));
    CHECK_FALSE(o.was_given("headless"));
    CHECK_THROWS_AS(parse({"--elf", "a.elf", "--module-dir", "m", "--status", "-", "--events", "-"}),
                    dq8::host::UsageError);
}

TEST_CASE("dq8-game settings: none without --config or --set; flags win over --set over --config",
          "[m5][host]") {
    CHECK_FALSE(dq8::host::load_settings(parse({"--elf", "a.elf", "--module-dir", "m"}), exe_dir));

    const dq8::test::TempDir tmp;
    const fs::path file = tmp.path() / "settings.toml";
    std::ofstream(file)
        << "schema_version = 1\n[audio]\noutput = \"null\"\nlatency_ms = 120\n"
           "[input]\nrumble = false\n[game]\nmovies = \"skip\"\nsave_dir = \"/saves\"\n"
           "memory_card2 = \"formatted\"\n[profile.TEST_00001.game]\nmemory_card2 = \"unformatted\"\n";

    // Every A row from the file.
    {
        dq8::host::Options o =
            parse({"--elf", "a.elf", "--module-dir", "m", "--config", file.string().c_str()});
        auto store = dq8::host::load_settings(o, exe_dir);
        REQUIRE(store);
        const std::vector<std::string> applied = dq8::host::apply_settings(o, *store);
        CHECK(applied == std::vector<std::string>{"audio.output=null", "audio.latency_ms=120",
                                                  "input.rumble=false", "game.movies=skip",
                                                  "game.save_dir=/saves", "game.memory_card2=formatted"});
        CHECK(o.audio == "null");
        CHECK(o.audio_latency_ms == 120);
        CHECK_FALSE(o.rumble);
        CHECK(o.movies == "skip");
        REQUIRE(o.save_dir);
        CHECK(*o.save_dir == "/saves");
        CHECK(o.memory_card2 == "formatted");
        // The profile's own table, once the profile is known.
        store->use_profile("TEST_00001");
        (void)dq8::host::apply_settings(o, *store);
        CHECK(o.memory_card2 == "unformatted");
    }
    // --set over the file, flags over both.
    {
        dq8::host::Options o =
            parse({"--elf", "a.elf", "--module-dir", "m", "--config", file.string().c_str(), "--set",
                   "game.movies=play", "--set", "audio.output=sdl", "--audio", "null", "--rumble", "on"});
        auto store = dq8::host::load_settings(o, exe_dir);
        REQUIRE(store);
        const std::vector<std::string> applied = dq8::host::apply_settings(o, *store);
        CHECK(o.movies == "play");
        CHECK(o.audio == "null"); // --audio null beat --set audio.output=sdl
        CHECK(o.rumble);          // --rumble on beat the file's false
        for (const std::string& a : applied) {
            CHECK(a.rfind("audio.output=", 0) != 0);
            CHECK(a.rfind("input.rumble=", 0) != 0);
        }
    }
    // An empty save folder keeps M4's default (unset); a missing file gives the defaults (M4's --window).
    {
        dq8::host::Options o = parse(
            {"--elf", "a.elf", "--module-dir", "m", "--config", (tmp.path() / "none.toml").string().c_str()});
        auto store = dq8::host::load_settings(o, exe_dir);
        REQUIRE(store);
        (void)dq8::host::apply_settings(o, *store);
        CHECK_FALSE(o.save_dir);
        CHECK(o.audio == "sdl");
        CHECK(o.audio_latency_ms == 64);
        CHECK(o.movies == "auto");
        CHECK(o.memory_card2 == "none");
        CHECK(o.rumble);
    }
    // A bad --set is an error; a bad value in the file is a warning.
    CHECK_THROWS_AS(
        dq8::host::load_settings(
            parse({"--elf", "a.elf", "--module-dir", "m", "--set", "audio.latency_ms=5"}), exe_dir),
        dq8::core::Error);
    std::ofstream(file, std::ios::trunc) << "[audio]\nlatency_ms = 5\n";
    auto store = dq8::host::load_settings(
        parse({"--elf", "a.elf", "--module-dir", "m", "--config", file.string().c_str()}), exe_dir);
    REQUIRE(store);
    REQUIRE(store->warnings().size() == 1);
    CHECK_THAT(store->warnings()[0], ContainsSubstring("audio.latency_ms = 5 is outside 10 to 1000"));
}

TEST_CASE("the status channel writes its five kinds of line", "[m5][host]") {
    const dq8::test::TempDir tmp;
    const fs::path file = tmp.path() / "status.jsonl";
    {
        dq8::host::StatusChannel status(file.string());
        REQUIRE(status.enabled());
        status.started("TEST_00001", true);
        status.guest_time(0.0); // starts the first second
        status.frame();
        status.frame(); // first_frame only once
        status.warning("settings-invalid", "a \"quoted\" message");
        status.exit(0, "vblanks");
        status.warning("late", "after the exit: dropped");
    }
    const std::vector<std::string> lines = lines_of(file);
    REQUIRE(lines.size() == 4);
    CHECK_THAT(lines[0], ContainsSubstring(R"({"ev":"started","version":")"));
    CHECK_THAT(lines[0], ContainsSubstring(R"("profile":"TEST_00001","pid":)"));
    CHECK_THAT(lines[0], ContainsSubstring(R"("window":true})"));
    CHECK_THAT(lines[1], ContainsSubstring(R"({"ev":"first_frame","ms":)"));
    CHECK(lines[2] == R"({"ev":"warning","code":"settings-invalid","message":"a \"quoted\" message"})");
    CHECK(lines[3] == R"({"ev":"exit","status":0,"reason":"vblanks"})");

    // An exception leaving run_game: the exit line carries its code and main()'s status.
    const fs::path errors = tmp.path() / "errors.jsonl";
    {
        dq8::host::StatusChannel status(errors.string());
        status.exit_on(
            std::make_exception_ptr(dq8::core::Error(dq8::core::ErrorCode::settings_parse, "bad file")));
    }
    const std::vector<std::string> error_lines = lines_of(errors);
    REQUIRE(error_lines.size() == 1);
    CHECK(error_lines[0] ==
          R"({"ev":"exit","status":2,"reason":"error","code":"settings-parse","message":"bad file"})");

    const dq8::host::StatusChannel none(std::nullopt);
    CHECK_FALSE(none.enabled());
}
