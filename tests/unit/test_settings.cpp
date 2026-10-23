// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// M5 session A: the settings schema, string tables and the layered store (dq8/core/settings.h).
#include "dq8/core/error.h"
#include "dq8/core/settings.h"
#include "support/iso_builder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <variant>

using Catch::Matchers::ContainsSubstring;
using dq8::core::Error;
using dq8::core::ErrorCode;
namespace settings = dq8::core::settings;
namespace fs = std::filesystem;

namespace {

const fs::path settings_dir = fs::path(DQ8_SOURCE_DIR) / "config" / "settings";

settings::Schema shipped_schema() {
    return settings::Schema::load(settings_dir / "schema.toml");
}

// A small schema of our own, so the tests do not depend on the shipped rows.
constexpr const char* small_schema = R"(schema_version = 2
[[setting]]
key = "display.mode"
type = "choice"
choices = ["a", "b"]
default = "a"
label = "display.mode.label"
description = "display.mode.description"
category = "display"
owner = "host"

[[setting]]
key = "display.width"
type = "integer"
min = 10
max = 100
default = 50
label = "display.width.label"
description = "display.width.description"
category = "display"
requires = ["display.mode=a"]
owner = "host"

[[setting]]
key = "audio.gain"
type = "real"
min = 0
max = 2.5
default = 1.0
label = "audio.gain.label"
description = "audio.gain.description"
category = "audio"
owner = "host"

[[setting]]
key = "audio.on"
type = "boolean"
default = true
label = "audio.on.label"
description = "audio.on.description"
category = "audio"
advanced = true
owner = "host"

[[setting]]
key = "game.dir"
type = "path"
default = ""
label = "game.dir.label"
description = "game.dir.description"
category = "game"
requires_restart = true
owner = "host"
)";

settings::Schema small() {
    return settings::Schema::parse(small_schema, "small.toml");
}

ErrorCode code_of(const std::function<void()>& f) {
    try {
        f();
    } catch (const Error& e) {
        return e.code();
    }
    FAIL("no error thrown");
    return ErrorCode::io_open;
}

std::string message_of(const std::function<void()>& f) {
    try {
        f();
    } catch (const Error& e) {
        return e.what();
    }
    return {};
}

// The small schema with one row's text replaced.
std::string schema_with(const std::string& from, const std::string& to) {
    std::string text = small_schema;
    const std::size_t at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

} // namespace

TEST_CASE("settings: the shipped schema loads with every M5 setting", "[m5][settings]") {
    const settings::Schema schema = shipped_schema();
    CHECK(schema.version() == 1);
    CHECK(schema.settings().size() == 19);
    CHECK(schema.categories() == std::vector<std::string>{"display", "audio", "input", "game"});
    const settings::Setting* width = schema.find("display.width");
    REQUIRE(width != nullptr);
    CHECK(width->type == settings::Type::integer);
    CHECK(width->min == 640);
    CHECK(width->max == 7680);
    CHECK(std::get<std::int64_t>(width->default_value) == 1280);
    CHECK(width->conditions == std::vector<std::string>{"display.window_mode=windowed"});
    CHECK(width->file_table() == "display");
    CHECK(width->file_name() == "width");
    const settings::Setting* movies = schema.find("game.movies");
    REQUIRE(movies != nullptr);
    CHECK(movies->choices == std::vector<std::string>{"auto", "play", "skip"});
    CHECK(movies->requires_restart);
    CHECK(schema.find("display.nothing") == nullptr);

    // Every text the schema names is in the English table.
    const settings::StringTable en = settings::StringTable::load(settings_dir / "strings" / "en.toml");
    for (const settings::Setting& s : schema.settings()) {
        INFO(s.key);
        CHECK(en.contains(s.label));
        CHECK(en.contains(s.description));
        CHECK(en.contains("category." + s.category));
        for (const std::string& c : s.choices) {
            CHECK(en.contains(s.key + ".choices." + c));
        }
    }
    CHECK(en.get("display.vsync.label") == "Vertical sync");
    CHECK(en.get("no.such.text") == "no.such.text");
}

TEST_CASE("settings: a schema that breaks its rules names the file and line", "[m5][settings]") {
    const auto parse = [](const std::string& text) {
        return [text] { (void)settings::Schema::parse(text, "s.toml"); };
    };
    CHECK(code_of(parse("schema_version = 1\n[[setting\n")) == ErrorCode::settings_parse);
    CHECK_THAT(message_of(parse("schema_version = 1\n[[setting\n")), ContainsSubstring("s.toml:2:"));
    CHECK(code_of(parse(schema_with("owner = \"host\"\n\n[[setting]]\nkey = \"display.width\"",
                                    "owner = \"host\"\n\n[[setting]]\nkey = \"display.mode\""))) ==
          ErrorCode::settings_invalid);
    CHECK_THAT(message_of(parse(schema_with("default = 50", "default = 500"))),
               ContainsSubstring("s.toml:17: display.width: the default is outside 10 to 100"));
    CHECK_THAT(message_of(parse(schema_with("default = \"a\"", "default = \"c\""))),
               ContainsSubstring("is not one of a or b"));
    CHECK_THAT(message_of(parse(schema_with("advanced = true", "advanced = true\ncolour = 1"))),
               ContainsSubstring("unknown field 'colour'"));
    CHECK_THAT(
        message_of(parse(schema_with("requires = [\"display.mode=a\"]", "requires = [\"display.mode=z\"]"))),
        ContainsSubstring("'requires' 'display.mode=z'"));
    CHECK_THAT(
        message_of(parse(schema_with("requires = [\"display.mode=a\"]", "requires = [\"nope.key=1\"]"))),
        ContainsSubstring("names 'nope.key'"));
    CHECK_THAT(message_of(parse(schema_with("key = \"audio.on\"", "key = \"Audio.On\""))),
               ContainsSubstring("is not a key"));
    CHECK_THAT(message_of(parse(schema_with("type = \"boolean\"", "type = \"colour\""))),
               ContainsSubstring("unknown type 'colour'"));
    CHECK(code_of(parse("schema_version = 0\n")) == ErrorCode::settings_invalid);
    CHECK(code_of([] { (void)settings::StringTable::parse("[strings]\nx = 1\n", "en.toml"); }) ==
          ErrorCode::settings_invalid);
}

TEST_CASE("settings: values come from the highest layer that holds them", "[m5][settings]") {
    settings::Store store(small());
    CHECK(store.get_string("display.mode") == "a");
    CHECK(store.source("display.mode") == settings::Layer::defaults);
    CHECK(store.get_real("audio.gain") == 1.0);

    store.load_text(settings::Layer::user,
                    "schema_version = 2\n[display]\nmode = \"b\"\nwidth = 20\n[audio]\ngain = 2\n"
                    "[profile.P1.display]\nwidth = 30\n[profile.P2.display]\nwidth = 40\n",
                    "user.toml");
    CHECK(store.warnings().empty());
    CHECK(store.get_string("display.mode") == "b");
    CHECK(store.get_int("display.width") == 20);
    CHECK(store.get_real("audio.gain") == 2.0); // an integer in the file reads as a real
    CHECK(store.source("display.width") == settings::Layer::user);

    store.use_profile("P1");
    CHECK(store.profile() == "P1");
    CHECK(store.get_int("display.width") == 30);
    CHECK(store.source("display.width") == settings::Layer::profile);
    store.use_profile("NONE");
    CHECK(store.get_int("display.width") == 20);
    store.use_profile("P2");
    CHECK(store.get_int("display.width") == 40);

    store.set_from_text("display.width=99");
    CHECK(store.get_int("display.width") == 99);
    CHECK(store.source("display.width") == settings::Layer::overrides);
    store.reset(settings::Layer::overrides, "display.width");
    CHECK(store.get_int("display.width") == 40);
    store.clear(settings::Layer::profile);
    CHECK(store.get_int("display.width") == 20);

    // requires: width applies only while mode is a.
    CHECK_FALSE(store.applies("display.width"));
    store.set(settings::Layer::overrides, "display.mode", std::string("a"));
    CHECK(store.applies("display.width"));

    // set and --set check their values and keys; file values never throw.
    CHECK(code_of([&] { store.set_from_text("display.width=101"); }) == ErrorCode::settings_invalid);
    CHECK(code_of([&] { store.set_from_text("display.mode=c"); }) == ErrorCode::settings_invalid);
    CHECK(code_of([&] { store.set_from_text("display.height=1"); }) == ErrorCode::settings_invalid);
    CHECK(code_of([&] { store.set_from_text("noequals"); }) == ErrorCode::settings_invalid);
    CHECK(code_of([&] { store.set(settings::Layer::user, "audio.on", std::string("yes")); }) ==
          ErrorCode::settings_invalid);
    CHECK(code_of([&] { store.set(settings::Layer::defaults, "audio.on", true); }) ==
          ErrorCode::settings_invalid);
    CHECK(code_of([&] { (void)store.get("audio.volume"); }) == ErrorCode::settings_invalid);
    store.set_from_text("audio.on=off");
    CHECK_FALSE(store.get_bool("audio.on"));
    store.set(settings::Layer::user, "audio.gain", std::int64_t{2}); // an integer for a real
    CHECK(std::holds_alternative<double>(store.get("audio.gain")));

    settings::Store no_profile(small());
    CHECK(code_of([&] { no_profile.set(settings::Layer::profile, "audio.on", true); }) ==
          ErrorCode::settings_invalid);
}

TEST_CASE("settings: a file's bad values and unknown keys are warnings, never errors", "[m5][settings]") {
    settings::Store store(small());
    store.load_text(
        settings::Layer::user,
        "schema_version = 9\nextra = 1\n[display]\nmode = \"z\"\nwidth = 5000\nshape = \"round\"\n"
        "[audio]\non = \"maybe\"\n[future]\nx = 1\n",
        "u.toml");
    const auto& w = store.warnings();
    REQUIRE(w.size() == 7);
    CHECK_THAT(w[0], ContainsSubstring("u.toml:1: written for schema_version 9"));
    CHECK_THAT(w[1], ContainsSubstring("u.toml:2: unknown key 'extra'"));
    CHECK_THAT(w[2], ContainsSubstring("u.toml:4: display.mode = \"z\" is not one of a or b"));
    CHECK_THAT(w[3], ContainsSubstring("u.toml:5: display.width = 5000 is outside 10 to 100"));
    CHECK_THAT(w[4], ContainsSubstring("u.toml:6: unknown setting 'display.shape'"));
    CHECK_THAT(w[5], ContainsSubstring("audio.on = \"maybe\" is not true or false"));
    CHECK_THAT(w[6], ContainsSubstring("unknown key 'future'"));
    // The defaults apply for the bad values.
    CHECK(store.get_string("display.mode") == "a");
    CHECK(store.get_int("display.width") == 50);
    CHECK(store.get_bool("audio.on"));
    CHECK(store.source("display.width") == settings::Layer::defaults);

    // Damaged TOML is an error with the line.
    CHECK(code_of([&] { store.load_text(settings::Layer::user, "[display\n", "bad.toml"); }) ==
          ErrorCode::settings_parse);
    CHECK(code_of([&] { store.load_text(settings::Layer::defaults, "", "x"); }) ==
          ErrorCode::settings_invalid);
}

TEST_CASE("settings: files round-trip with their unknown keys, comments and profiles", "[m5][settings]") {
    const std::string original = "# my settings\n"
                                 "schema_version = 2\n"
                                 "note = \"keep\"\n"
                                 "\n"
                                 "[display]\n"
                                 "# my width\n"
                                 "width = 20\n"
                                 "shape = \"round\"\n"
                                 "\n"
                                 "[future]\n"
                                 "x = 1\n"
                                 "\n"
                                 "[profile.P1.audio]\n"
                                 "on = false\n"
                                 "later = 2\n";
    settings::Store store(small());
    store.load_text(settings::Layer::user, original, "u.toml");
    store.set(settings::Layer::user, "display.width", std::int64_t{70});
    store.set(settings::Layer::user, "audio.gain", 0.5);
    store.use_profile("P2");
    store.set(settings::Layer::profile, "display.mode", std::string("b"));
    const std::string saved = store.to_toml(settings::Layer::user);
    INFO(saved);
    CHECK_THAT(saved, ContainsSubstring("# my settings"));
    CHECK_THAT(saved, ContainsSubstring("# my width"));
    CHECK_THAT(saved, ContainsSubstring("note = \"keep\""));
    CHECK_THAT(saved, ContainsSubstring("shape = \"round\""));
    CHECK_THAT(saved, ContainsSubstring("[future]"));
    CHECK_THAT(saved, ContainsSubstring("later = 2"));
    CHECK_THAT(saved, ContainsSubstring("width = 70"));
    CHECK_THAT(saved, ContainsSubstring("gain = 0.5"));
    CHECK_THAT(saved, ContainsSubstring("[profile.P2.display]"));
    CHECK(saved.find("[profile]\n") == std::string::npos);
    CHECK(saved.find("schema_version = 2") < saved.find("[display]"));

    // Reading the saved text back gives the same values and keeps the unknown keys again.
    settings::Store again(small());
    again.load_text(settings::Layer::user, saved, "saved.toml");
    CHECK(again.get_int("display.width") == 70);
    CHECK(again.get_real("audio.gain") == 0.5);
    again.use_profile("P1");
    CHECK_FALSE(again.get_bool("audio.on"));
    again.use_profile("P2");
    CHECK(again.get_string("display.mode") == "b");
    CHECK(again.to_toml(settings::Layer::user) == saved);

    // A reset key leaves the file; an emptied profile disappears.
    again.reset(settings::Layer::profile, "display.mode");
    again.reset(settings::Layer::user, "display.width");
    const std::string reset = again.to_toml(settings::Layer::user);
    CHECK(reset.find("width =") == std::string::npos);
    CHECK(reset.find("[profile.P2") == std::string::npos);
    CHECK_THAT(reset, ContainsSubstring("shape = \"round\""));

    // Through files, atomically.
    const dq8::test::TempDir tmp;
    const fs::path file = tmp.path() / "sub" / "settings.toml";
    store.save(settings::Layer::user, file);
    settings::Store from_file(small());
    from_file.load(settings::Layer::user, file);
    CHECK(from_file.get_int("display.width") == 70);
    // A missing file is an empty layer, and saving it starts a new file.
    settings::Store fresh(small());
    fresh.load(settings::Layer::user, tmp.path() / "missing.toml");
    CHECK(fresh.warnings().empty());
    const std::string fresh_text = fresh.to_toml(settings::Layer::user);
    CHECK_THAT(fresh_text, ContainsSubstring("schema_version = 2"));
    CHECK_THAT(fresh_text, ContainsSubstring("# DQ8-Native settings"));
}

TEST_CASE("settings: the defaults file lists every setting and reads back as the defaults",
          "[m5][settings]") {
    const settings::Schema schema = shipped_schema();
    settings::Store store(schema);
    const std::string text = store.to_toml(settings::Layer::defaults);
    settings::Store again(schema);
    again.load_text(settings::Layer::user, text, "defaults.toml");
    CHECK(again.warnings().empty());
    for (const settings::Setting& s : schema.settings()) {
        INFO(s.key);
        CHECK(again.source(s.key) == settings::Layer::user);
        CHECK(again.get(s.key) == s.default_value);
    }
}

TEST_CASE("settings: text forms of values", "[m5][settings]") {
    const settings::Schema schema = small();
    const settings::Setting& gain = *schema.find("audio.gain");
    const settings::Setting& on = *schema.find("audio.on");
    const settings::Setting& mode = *schema.find("display.mode");
    CHECK(settings::to_text(settings::parse_value(gain, "1.25")) == "1.25");
    CHECK(settings::to_text(settings::parse_value(gain, "2")) == "2.0");
    CHECK(settings::to_text(settings::parse_value(on, "yes")) == "true");
    CHECK(settings::to_text(settings::parse_value(on, "0")) == "false");
    CHECK(settings::to_text(settings::Value{std::int64_t{-3}}) == "-3");
    CHECK(settings::describe_allowed(mode) == "a or b");
    CHECK(settings::describe_allowed(gain) == "0.0 to 2.5");
    CHECK_THAT(message_of([&] { (void)settings::parse_value(gain, "3"); }),
               ContainsSubstring("audio.gain = '3' is outside 0.0 to 2.5"));
    CHECK(code_of([&] { (void)settings::parse_value(gain, "x"); }) == ErrorCode::settings_invalid);
    CHECK(settings::check_value(mode, std::string("b")).empty());
    CHECK_FALSE(settings::check_value(mode, true).empty());
}

TEST_CASE(
    "settings: the schema is found from the build tree; a portable marker keeps the file beside the program",
    "[m5][settings]") {
    const fs::path dir = settings::find_settings_dir(fs::path(DQ8_CONTRACT_MODULE_DIR));
    CHECK(fs::exists(dir / "schema.toml"));
    const dq8::test::TempDir tmp;
    CHECK(settings::default_user_file(tmp.path()).filename() == "settings.toml");
    CHECK(settings::default_user_file(tmp.path()).parent_path() != tmp.path());
    std::ofstream(tmp.path() / "portable") << "";
    CHECK(settings::default_user_file(tmp.path()) == tmp.path() / "settings.toml");
}
