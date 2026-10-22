// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// M5 session C without SDL: the input-profile format (dq8/core/input_profile.h), the region profile's
// [display] table and the display watcher, dq8-game's display, input and audio options and what the host
// makes of them (src/host/display_wiring.*). Every address and value is ours or the shipped profile's.
#include "dq8/core/error.h"
#include "dq8/core/input_profile.h"
#include "dq8/core/region_profile.h"
#include "game/display_watcher.h"
#include "host/display_wiring.h"
#include "host/options.h"
#include "support/fake_machine.h"
#include "support/iso_builder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace dq8;
using Catch::Matchers::ContainsSubstring;
namespace input = core::input;

const std::filesystem::path shipped_profile =
    std::filesystem::path(DQ8_SOURCE_DIR) / "data" / "regions" / "SLUS_21207.toml";

std::string read_text(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

std::filesystem::path write(const std::filesystem::path& dir, const std::string& name,
                            const std::string& text) {
    const auto file = dir / name;
    std::ofstream(file, std::ios::binary) << text;
    return file;
}

// The shipped profile with its [display] table replaced by `table` (empty: none).
std::string with_display(const std::string& table) {
    std::string text = read_text(shipped_profile);
    const std::size_t at = text.find("[display]\n");
    REQUIRE(at != std::string::npos);
    const std::size_t end = text.find("\n\n", at);
    return text.substr(0, at) + table + text.substr(end + 1);
}

host::Options parse(std::initializer_list<const char*> args) {
    std::vector<const char*> argv{"dq8-game", "--iso", "x.iso", "--module-dir", "m"};
    argv.insert(argv.end(), args.begin(), args.end());
    return host::parse_options(static_cast<int>(argv.size()), argv.data());
}

std::string error_of(std::initializer_list<const char*> args) {
    try {
        parse(args);
    } catch (const host::UsageError& e) {
        return e.what();
    }
    return "<accepted>";
}

} // namespace

TEST_CASE("input profiles: the built-in profile round-trips through the format", "[display][input]") {
    const input::Profile defaults = input::default_profile();
    CHECK(defaults.keyboard[input::Control::cross] == std::vector<std::string>{"K"});
    CHECK(defaults.gamepad[input::Control::cross] == std::vector<std::string>{"a"});
    CHECK(defaults.gamepad[input::Control::left_stick_left] == std::vector<std::string>{"leftx-"});
    for (std::size_t i = 0; i < input::control_count; ++i) {
        CHECK_FALSE(defaults.keyboard.sources[i].empty());
        CHECK_FALSE(defaults.gamepad.sources[i].empty());
        CHECK(input::control_from_name(input::control_name(input::control_at(i))) == input::control_at(i));
    }
    CHECK_FALSE(input::control_from_name("jump"));

    const std::string text = input::format_profile(defaults);
    CHECK(input::parse_profile(text, "defaults.toml") == defaults);
    // Only the version: everything is the built-in bindings.
    CHECK(input::parse_profile("schema_version = 1\n", "empty.toml") == defaults);
}

TEST_CASE("input profiles: tables replace the controls they list, devices override [gamepad]",
          "[display][input]") {
    const std::string text = R"(schema_version = 1
[keyboard]
cross = ["Space", "K"]
circle = []

[gamepad]
cross = ["b"]

[devices."0300000000000000000000000000ab01"]
name = "Test \"pad\""
port = 2
cross = ["x"]
left_stick_up = ["righty-"]
)";
    const input::Profile p = input::parse_profile(text, "custom.toml");
    CHECK(p.keyboard[input::Control::cross] == std::vector<std::string>{"Space", "K"});
    CHECK(p.keyboard[input::Control::circle].empty());                          // unbound
    CHECK(p.keyboard[input::Control::square] == std::vector<std::string>{"J"}); // not listed: built-in
    CHECK(p.gamepad[input::Control::cross] == std::vector<std::string>{"b"});
    REQUIRE(p.devices.size() == 1);
    CHECK(p.devices[0].name == "Test \"pad\"");
    CHECK(p.devices[0].port == 1u);

    const input::Bindings other = p.gamepad_for("030000000000000000000000000000ff");
    CHECK(other == p.gamepad);
    const input::Bindings own = p.gamepad_for("0300000000000000000000000000ab01");
    CHECK(own[input::Control::cross] == std::vector<std::string>{"x"});
    CHECK(own[input::Control::left_stick_up] == std::vector<std::string>{"righty-"});
    CHECK(own[input::Control::circle] == p.gamepad[input::Control::circle]);

    // The writer keeps everything, the device's quotes, backslashes and control characters included.
    CHECK(input::parse_profile(input::format_profile(p), "again.toml") == p);
    input::Profile odd = p;
    odd.devices[0].name = "tab\there\\ \x7f end";
    CHECK(input::parse_profile(input::format_profile(odd), "odd.toml") == odd);
}

TEST_CASE("input profiles: problems name the file and line", "[display][input]") {
    const auto fails = [](const std::string& text, const std::string& what, core::ErrorCode code) {
        try {
            input::parse_profile(text, "bad.toml");
            FAIL("accepted: " << text);
        } catch (const core::Error& e) {
            CHECK(e.code() == code);
            CHECK_THAT(e.what(), ContainsSubstring("bad.toml:") && ContainsSubstring(what));
        }
    };
    fails("schema_version = 1\n[keyboard\n", "", core::ErrorCode::settings_parse);
    fails("[keyboard]\ncross = [\"K\"]\n", "schema_version", core::ErrorCode::settings_invalid);
    fails("schema_version = 2\n", "not supported", core::ErrorCode::settings_invalid);
    fails("schema_version = 1\n[keyboard]\njump = [\"K\"]\n", "not a pad control",
          core::ErrorCode::settings_invalid);
    fails("schema_version = 1\n[keyboard]\ncross = \"K\"\n", "array", core::ErrorCode::settings_invalid);
    fails("schema_version = 1\n[keyboard]\ncross = [\"\"]\n", "source names",
          core::ErrorCode::settings_invalid);
    fails("schema_version = 1\n[mouse]\n", "unknown key", core::ErrorCode::settings_invalid);
    fails("schema_version = 1\n[devices.\"xyz\"]\n", "GUID", core::ErrorCode::settings_invalid);
    fails("schema_version = 1\n[devices.\"0300000000000000000000000000ab01\"]\nport = 0\n", "1 or 2",
          core::ErrorCode::settings_invalid);

    CHECK(input::valid_profile_name("default"));
    CHECK(input::valid_profile_name("Pad-2_b"));
    CHECK_FALSE(input::valid_profile_name(""));
    CHECK_FALSE(input::valid_profile_name("../x"));
    CHECK_FALSE(input::valid_profile_name("a b"));
    CHECK(input::profile_file("/cfg", "mine") == std::filesystem::path("/cfg/input/mine.toml"));
}

TEST_CASE("input profiles: saved files load back, missing ones are io_open", "[display][input]") {
    test::TempDir tmp;
    input::Profile p = input::default_profile();
    p.keyboard[input::Control::start] = {"Space"};
    const auto file = input::profile_file(tmp.path(), "saved");
    input::save_profile(file, p); // creates input/
    CHECK(input::load_profile(file) == p);
    try {
        input::load_profile(tmp.path() / "none.toml");
        FAIL("loaded a missing file");
    } catch (const core::Error& e) {
        CHECK(e.code() == core::ErrorCode::io_open);
    }
}

TEST_CASE("region profile: the [display] table", "[display][profile]") {
    const core::RegionProfile shipped = core::load_region_profile(shipped_profile);
    CHECK(shipped.display.widescreen_variable == 0x003D275C);
    CHECK(shipped.display.widescreen_width == 1);
    CHECK(shipped.display.widescreen_value == 1);

    test::TempDir tmp;
    const auto none = core::load_region_profile(write(tmp.path(), "none.toml", with_display("")));
    CHECK(none.display.widescreen_variable == 0);

    const auto fails = [&tmp](const std::string& table, const std::string& what) {
        const auto file = write(tmp.path(), "bad.toml", with_display(table));
        CHECK_THROWS_WITH(core::load_region_profile(file), ContainsSubstring(what));
    };
    fails("[display]\nwidescreen_variable = 0x003D275C\nwidescreen_width = 3\nwidescreen_value = 1\n",
          "1, 2 or 4");
    fails("[display]\nwidescreen_variable = 0x003D275D\nwidescreen_width = 2\nwidescreen_value = 1\n",
          "aligned to its width");
    fails("[display]\nwidescreen_variable = 0x00500000\nwidescreen_width = 1\nwidescreen_value = 1\n",
          "inside the main program");
    fails("[display]\nwidescreen_variable = 0x003D275C\nwidescreen_width = 1\nwidescreen_value = 256\n",
          "does not fit");
    fails("[display]\nwidescreen_variable = 0x003D275C\nwidescreen_value = 1\n", "widescreen_width");
}

TEST_CASE("display watcher: follows the screen-size variable without an event", "[display][game]") {
    test::FakeMachine machine;
    const core::RegionProfile::Display display{0x00200000, 2, 0x0101};
    game::DisplayWatcher watcher(display);
    machine.attach(watcher);
    const auto set = [&](std::uint16_t v) {
        const std::uint8_t b[2] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8)};
        machine.memory().write_bytes(0x00200000, b);
    };
    machine.fire_vblank_start();
    CHECK_FALSE(watcher.widescreen()); // 0, as bss starts
    set(0x0101);
    CHECK_FALSE(watcher.widescreen()); // read at vblank start only
    machine.fire_vblank_start();
    CHECK(watcher.widescreen());
    set(0x0001);
    machine.fire_vblank_start();
    CHECK_FALSE(watcher.widescreen()); // the whole width counts
    CHECK(watcher.changes() == 2);
    CHECK(machine.emitted.empty());
    watcher.reset();
    CHECK(watcher.changes() == 0);

    CHECK_THROWS_AS(game::DisplayWatcher(core::RegionProfile::Display{}), std::invalid_argument);
    CHECK_THROWS_AS(game::DisplayWatcher(core::RegionProfile::Display{0x00200001, 2, 1}),
                    std::invalid_argument);
}

TEST_CASE("host options: the display, input and audio options", "[display][host]") {
    const host::Options defaults = parse({});
    CHECK(defaults.window_mode == "windowed");
    CHECK(defaults.window_width == 1280);
    CHECK(defaults.window_height == 960);
    CHECK(defaults.vsync);
    CHECK(defaults.aspect == "auto");
    CHECK_FALSE(defaults.integer_scaling);
    CHECK(defaults.filter == "linear");
    CHECK(defaults.internal_resolution == 1);
    CHECK_FALSE(defaults.pipeline_cache);
    CHECK(defaults.audio_device.empty());
    CHECK(defaults.volume == 100);
    CHECK(defaults.input_profile == "default");
    CHECK(defaults.deadzone == 8);
    CHECK(defaults.rumble_strength == 100);
    CHECK_FALSE(defaults.uses_machine);

    const host::Options o = parse({"--window",   "--window-mode",
                                   "fullscreen", "--window-size",
                                   "1920x1080",  "--vsync",
                                   "off",        "--aspect",
                                   "16:9",       "--integer-scaling",
                                   "on",         "--filter",
                                   "nearest",    "--internal-resolution",
                                   "3x",         "--pipeline-cache",
                                   "none",       "--audio-device",
                                   "USB Audio",  "--volume",
                                   "35",         "--input-profile",
                                   "pad2",       "--deadzone",
                                   "20",         "--rumble-strength",
                                   "0"});
    CHECK(o.window_mode == "fullscreen");
    CHECK(o.window_width == 1920);
    CHECK(o.window_height == 1080);
    CHECK_FALSE(o.vsync);
    CHECK(o.aspect == "16:9");
    CHECK(o.integer_scaling);
    CHECK(o.filter == "nearest");
    CHECK(o.internal_resolution == 3);
    CHECK(o.pipeline_cache == "none");
    CHECK(o.audio_device == "USB Audio");
    CHECK(o.volume == 35);
    CHECK(o.input_profile == "pad2");
    CHECK(o.deadzone == 20);
    CHECK(o.rumble_strength == 0);
    CHECK(parse({"--aspect", "stretch"}).uses_machine);

    CHECK_THAT(error_of({"--window-mode", "tv"}), ContainsSubstring("windowed, borderless, fullscreen"));
    CHECK_THAT(error_of({"--window-size", "1280"}), ContainsSubstring("WIDTHxHEIGHT"));
    CHECK_THAT(error_of({"--window-size", "100x100"}), ContainsSubstring("320x240 to 7680x4320"));
    CHECK_THAT(error_of({"--vsync", "yes"}), ContainsSubstring("on, off"));
    CHECK_THAT(error_of({"--aspect", "21:9"}), ContainsSubstring("auto, 4:3, 16:9, stretch"));
    CHECK_THAT(error_of({"--internal-resolution", "8x", "--gs", "parallel"}),
               ContainsSubstring("1x, 2x, 3x, 4x"));
    CHECK_THAT(error_of({"--internal-resolution", "2x"}), ContainsSubstring("--gs parallel"));
    CHECK_THAT(error_of({"--internal-resolution", "2x", "--gs", "parallel", "--ssaa", "4"}),
               ContainsSubstring("--ssaa and --internal-resolution"));
    CHECK_THAT(error_of({"--volume", "101"}), ContainsSubstring("0 to 100"));
    CHECK_THAT(error_of({"--deadzone", "51"}), ContainsSubstring("0 to 50"));
    CHECK_THAT(error_of({"--rumble-strength", "-1"}), ContainsSubstring("not a number"));
    CHECK_THAT(error_of({"--volume", "50", "--volume", "60"}), ContainsSubstring("given twice"));

    const host::Options dumps =
        parse({"--window", "--dump-window", "100:a.png", "--dump-window", "200:b.png"});
    REQUIRE(dumps.dump_window.size() == 2);
    CHECK(dumps.dump_window[1].vblank == 200);
    CHECK(dumps.dump_window[1].path == std::filesystem::path("b.png"));
    CHECK_THAT(error_of({"--dump-window", "100:a.png"}), ContainsSubstring("needs --window"));
}

TEST_CASE("display wiring: internal resolution, pipeline cache, aspect and the input profile",
          "[display][host]") {
    CHECK(host::internal_resolution(1).super_sampling == 1);
    CHECK_FALSE(host::internal_resolution(1).high_resolution);
    CHECK(host::internal_resolution(2).super_sampling == 4);
    CHECK(host::internal_resolution(2).high_resolution);
    CHECK(host::internal_resolution(3).super_sampling == 8);
    CHECK(host::internal_resolution(4).super_sampling == 16);

    host::Options o = parse({});
    CHECK(host::pipeline_cache_file(o).empty()); // headless: none
    o.display = host::Display::window;
    CHECK(host::pipeline_cache_file(o).filename() == "parallel-gs.pipelines");
    o.pipeline_cache = "none";
    CHECK(host::pipeline_cache_file(o).empty());
    o.pipeline_cache = "/tmp/x.bin";
    CHECK(host::pipeline_cache_file(o) == std::filesystem::path("/tmp/x.bin"));

    test::FakeMachine machine;
    game::DisplayWatcher watcher(core::RegionProfile::Display{0x00200000, 1, 1});
    machine.attach(watcher);
    host::Options a = parse({});
    CHECK(host::present_aspect(a, nullptr) == 4.0 / 3.0);
    CHECK(host::present_aspect(a, &watcher) == 4.0 / 3.0);
    const std::uint8_t one[1] = {1};
    machine.memory().write_bytes(0x00200000, one);
    machine.fire_vblank_start();
    CHECK(host::present_aspect(a, &watcher) == 16.0 / 9.0);
    a.aspect = "4:3";
    CHECK(host::present_aspect(a, &watcher) == 4.0 / 3.0);
    a.aspect = "16:9";
    CHECK(host::present_aspect(a, nullptr) == 16.0 / 9.0);
    a.aspect = "stretch";
    CHECK(host::present_aspect(a, &watcher) == 0.0);

    host::Options p = parse({});
    CHECK(host::input_profile_file(p, "/cfg") == std::filesystem::path("/cfg/input/default.toml"));
    p.input_profile = "elsewhere/pad.toml";
    CHECK(host::input_profile_file(p, "/cfg") == std::filesystem::path("elsewhere/pad.toml"));
    p.input_profile = "bad name";
    CHECK_THROWS_AS(host::input_profile_file(p, "/cfg"), host::UsageError);

    // A missing file is the built-in bindings: silently for "default", with a warning otherwise; a broken
    // file too, with the parser's message.
    test::TempDir tmp;
    std::vector<std::string> warnings;
    p.input_profile = (tmp.path() / "missing.toml").string();
    CHECK(host::load_input_profile(p, warnings) == input::default_profile());
    REQUIRE(warnings.size() == 1);
    CHECK_THAT(warnings[0], ContainsSubstring("no input profile"));
    warnings.clear();
    p.input_profile =
        write(tmp.path(), "broken.toml", "schema_version = 1\n[keyboard]\njump = []\n").string();
    CHECK(host::load_input_profile(p, warnings) == input::default_profile());
    REQUIRE(warnings.size() == 1);
    CHECK_THAT(warnings[0], ContainsSubstring("not a pad control"));
    warnings.clear();
    p.input_profile =
        write(tmp.path(), "good.toml", "schema_version = 1\n[keyboard]\nstart = [\"Space\"]\n").string();
    p.deadzone = 15;
    p.rumble_strength = 40;
    p.window_mode = "borderless";
    p.filter = "nearest";
    const rt::WindowConfig c = host::window_config(p, warnings);
    CHECK(warnings.empty());
    CHECK(c.input.profile.keyboard[input::Control::start] == std::vector<std::string>{"Space"});
    CHECK(c.input.deadzone_percent == 15);
    CHECK(c.input.rumble_percent == 40);
    CHECK(c.mode == rt::WindowMode::borderless);
    CHECK(c.filter == rt::ScaleFilter::nearest);
    CHECK(c.width == 1280);
}
