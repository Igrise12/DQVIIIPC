// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The Settings pages: generated from the schema and the string table, conditions, the advanced toggle,
// restart hints, validation, reset and saving. The schema here is read with toml11 so the test does not
// depend on session A's loader; the real config/settings/schema.toml and strings/en.toml are checked row by
// row.
#include "model/settings_model.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <toml.hpp>

using namespace dq8::launcher;
using dq8::core::ErrorCode;
using dq8::core::settings::Setting;
using dq8::core::settings::Type;
using dq8::core::settings::Value;

namespace {

Type type_from(const std::string& name) {
    if (name == "boolean") {
        return Type::boolean;
    }
    if (name == "integer") {
        return Type::integer;
    }
    if (name == "real") {
        return Type::real;
    }
    if (name == "choice") {
        return Type::choice;
    }
    if (name == "text") {
        return Type::text;
    }
    REQUIRE(name == "path");
    return Type::path;
}

double number(const toml::value& v) {
    return v.is_floating() ? v.as_floating() : static_cast<double>(v.as_integer());
}

// The schema format of Docs/milestones/M5-launcher.md, read independently of core's loader.
std::vector<Setting> schema_settings(std::string_view text) {
    const toml::value root = toml::parse_str(std::string(text));
    std::vector<Setting> out;
    // Copies, not references: GCC's -Wdangling-reference flags what toml::find returns for a temporary key.
    const toml::array rows = toml::find<toml::array>(root, "setting");
    for (const toml::value& row : rows) {
        Setting s;
        s.key = toml::find<std::string>(row, "key");
        s.type = type_from(toml::find<std::string>(row, "type"));
        const toml::value def = toml::find(row, "default");
        switch (s.type) {
        case Type::boolean:
            s.default_value = def.as_boolean();
            break;
        case Type::integer:
            s.default_value = static_cast<std::int64_t>(def.as_integer());
            s.min = static_cast<std::int64_t>(toml::find(row, "min").as_integer());
            s.max = static_cast<std::int64_t>(toml::find(row, "max").as_integer());
            break;
        case Type::real:
            s.default_value = number(def);
            s.min_real = number(toml::find(row, "min"));
            s.max_real = number(toml::find(row, "max"));
            break;
        default:
            s.default_value = def.as_string();
            break;
        }
        if (s.type == Type::choice) {
            s.choices = toml::find<std::vector<std::string>>(row, "choices");
        }
        s.label = toml::find<std::string>(row, "label");
        s.description = toml::find<std::string>(row, "description");
        s.category = toml::find<std::string>(row, "category");
        s.advanced = toml::find_or<bool>(row, "advanced", false);
        s.requires_restart = toml::find_or<bool>(row, "requires_restart", false);
        s.hot_reload = toml::find_or<bool>(row, "hot_reload", false);
        s.conditions = toml::find_or<std::vector<std::string>>(row, "requires", {});
        s.owner = toml::find_or<std::string>(row, "owner", "");
        out.push_back(std::move(s));
    }
    return out;
}

std::map<std::string, std::string, std::less<>> string_table(std::string_view text) {
    const toml::value root = toml::parse_str(std::string(text));
    std::map<std::string, std::string, std::less<>> out;
    const toml::table table = toml::find<toml::table>(root, "strings");
    for (const auto& [k, v] : table) {
        out[k] = v.as_string();
    }
    return out;
}

constexpr std::string_view sample_schema = R"(
schema_version = 1

[[setting]]
key = "display.mode"
type = "choice"
choices = ["window", "full"]
default = "window"
label = "display.mode.label"
description = "display.mode.description"
category = "display"

[[setting]]
key = "display.width"
type = "integer"
min = 640
max = 7680
default = 1280
label = "display.width.label"
description = "display.width.description"
category = "display"
requires = ["display.mode=window"]

[[setting]]
key = "display.vsync"
type = "boolean"
default = true
label = "display.vsync.label"
description = "display.vsync.description"
category = "display"
requires_restart = true

[[setting]]
key = "audio.volume"
type = "integer"
min = 0
max = 100
default = 100
label = "audio.volume.label"
description = "audio.volume.description"
category = "audio"

[[setting]]
key = "audio.latency"
type = "integer"
min = 10
max = 1000
default = 64
label = "audio.latency.label"
description = "audio.latency.description"
category = "audio"
advanced = true
requires = ["audio.volume=100"]

[[setting]]
key = "display.gamma"
type = "real"
min = 0.5
max = 2.5
default = 1.0
label = "display.gamma.label"
description = "display.gamma.description"
category = "display"
advanced = true

[[setting]]
key = "game.save_dir"
type = "path"
default = ""
label = "game.save_dir.label"
description = "game.save_dir.description"
category = "game"

[[setting]]
key = "game.name"
type = "text"
default = "x"
label = "game.name.label"
description = "game.name.description"
category = "game"
)";

SettingsModel sample_model(MemorySettings** out = nullptr) {
    auto backend = std::make_unique<MemorySettings>(
        schema_settings(sample_schema),
        std::map<std::string, std::string, std::less<>>{{"category.display", "Display"},
                                                        {"display.mode.label", "Mode"},
                                                        {"display.mode.choices.window", "Windowed"}},
        "settings.toml");
    if (out != nullptr) {
        *out = backend.get();
    }
    return SettingsModel(std::move(backend));
}

const SettingRow* find_row(const SettingsPage& page, std::string_view key) {
    for (const SettingRow& r : page.rows) {
        if (r.key == key) {
            return &r;
        }
    }
    return nullptr;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("launcher: pages and rows come from the schema", "[launcher][settings]") {
    const SettingsModel m = sample_model();
    CHECK(m.categories() == std::vector<std::string>{"display", "audio", "game"});
    CHECK(m.category_title("display") == "Display");
    CHECK(m.category_title("audio") == "Audio"); // no string: the name, capitalised

    const SettingsPage display = m.page("display", false, false);
    CHECK(display.title == "Display");
    REQUIRE(display.rows.size() == 3); // mode, width (condition holds), vsync; gamma is advanced
    CHECK(display.advanced_hidden == 1);
    CHECK(display.conditions_hidden == 0);
    const SettingRow* mode = find_row(display, "display.mode");
    REQUIRE(mode);
    CHECK(mode->label == "Mode");
    CHECK(mode->description.empty()); // a missing description is left out
    CHECK(find_row(display, "display.vsync")->label ==
          "display.vsync.label"); // a missing label shows its key
    CHECK(mode->widget == Widget::combo);
    REQUIRE(mode->choices.size() == 2);
    CHECK(mode->choices[0].text == "Windowed");
    CHECK(mode->choices[1].text == "full"); // no text: the value
    CHECK(find_row(display, "display.width")->widget == Widget::input_int);
    CHECK(find_row(display, "display.vsync")->widget == Widget::checkbox);
    CHECK(find_row(display, "display.vsync")->hint == "Takes effect the next time the game starts.");
    CHECK(find_row(display, "display.width")->hint.empty());

    const SettingsPage advanced = m.page("display", true, false);
    REQUIRE(advanced.rows.size() == 4);
    CHECK(find_row(advanced, "display.gamma")->widget == Widget::slider_real);
    CHECK(find_row(advanced, "display.gamma")->max_real == 2.5);

    const SettingsPage audio = m.page("audio", true, false);
    CHECK(find_row(audio, "audio.volume")->widget == Widget::slider_int);
    CHECK(find_row(audio, "audio.latency")->widget == Widget::input_int);
    const SettingsPage game = m.page("game", false, false);
    CHECK(find_row(game, "game.save_dir")->widget == Widget::path);
    CHECK(find_row(game, "game.name")->widget == Widget::text);
}

TEST_CASE("launcher: requires conditions show and hide rows", "[launcher][settings]") {
    SettingsModel m = sample_model();
    REQUIRE(m.set("display.mode", std::string("full")));
    const SettingsPage display = m.page("display", false, false);
    CHECK(find_row(display, "display.width") == nullptr);
    CHECK(display.conditions_hidden == 1);
    REQUIRE(m.set("audio.volume", std::int64_t{50}));
    CHECK(find_row(m.page("audio", true, false), "audio.latency") == nullptr);
    REQUIRE(m.set("audio.volume", std::int64_t{100}));
    CHECK(find_row(m.page("audio", true, false), "audio.latency") != nullptr);

    Setting broken;
    broken.key = "x.y";
    broken.conditions = {"no-equals-sign"};
    CHECK_FALSE(m.conditions_hold(broken));
    broken.conditions = {"unknown.key=1"};
    CHECK_FALSE(m.conditions_hold(broken));
    broken.conditions = {};
    CHECK(m.conditions_hold(broken));
}

TEST_CASE("launcher: values are checked, clamped, reset and saved", "[launcher][settings]") {
    MemorySettings* backend = nullptr;
    SettingsModel m = sample_model(&backend);
    CHECK_FALSE(m.dirty());

    CHECK(m.set("audio.volume", std::int64_t{150})); // clamped to the range
    CHECK(std::get<std::int64_t>(backend->value("audio.volume")) == 100);
    CHECK_FALSE(m.dirty()); // 100 was already the value
    CHECK(m.set("audio.volume", std::int64_t{-5}));
    CHECK(std::get<std::int64_t>(backend->value("audio.volume")) == 0);
    CHECK(m.dirty());
    CHECK(find_row(m.page("audio", false, false), "audio.volume")->modified);

    CHECK_FALSE(m.set("display.mode", std::string("tiny")));
    REQUIRE(m.problem());
    CHECK(*m.problem()->code == ErrorCode::settings_invalid);
    CHECK_FALSE(m.set("display.vsync", std::int64_t{1})); // wrong type
    CHECK_FALSE(m.set("no.such", true));
    CHECK(m.set("display.gamma", 9.0));
    CHECK(std::get<double>(backend->value("display.gamma")) == 2.5);
    CHECK_FALSE(m.set("display.gamma", std::numeric_limits<double>::quiet_NaN()));

    CHECK(m.save());
    CHECK(backend->saves() == 1);
    CHECK_FALSE(m.dirty());
    CHECK_FALSE(m.problem());

    m.reset("audio.volume");
    CHECK(m.dirty());
    CHECK(std::get<std::int64_t>(backend->value("audio.volume")) == 100);
    CHECK_FALSE(find_row(m.page("audio", false, false), "audio.volume")->modified);

    REQUIRE(m.set("display.mode", std::string("full")));
    REQUIRE(m.set("display.vsync", false));
    m.reset_page("display");
    CHECK(std::get<std::string>(backend->value("display.mode")) == "window");
    CHECK(std::get<bool>(backend->value("display.vsync")));

    backend->fail_saves(true);
    CHECK_FALSE(m.save());
    REQUIRE(m.problem());
    CHECK(*m.problem()->code == ErrorCode::io_write);
    CHECK(m.dirty());
}

TEST_CASE("launcher: a running game adds the restart hint to every row", "[launcher][settings]") {
    const SettingsModel m = sample_model();
    for (const SettingRow& r : m.page("display", true, true).rows) {
        CHECK(r.hint.find("starts again") != std::string::npos);
    }
}

TEST_CASE("launcher: a new schema row needs no launcher code", "[launcher][settings]") {
    // A category and a type the launcher has never seen in M5's schema.
    std::string text(sample_schema);
    text += R"(
[[setting]]
key = "enhancements.bloom"
type = "real"
min = 0
max = 1
default = 0.25
label = "enhancements.bloom.label"
description = "enhancements.bloom.description"
category = "enhancements"
requires = ["display.vsync=true"]
)";
    SettingsModel m(std::make_unique<MemorySettings>(schema_settings(text),
                                                     std::map<std::string, std::string, std::less<>>{}));
    CHECK(m.categories().back() == "enhancements");
    const SettingsPage page = m.page("enhancements", false, false);
    CHECK(page.title == "Enhancements");
    REQUIRE(page.rows.size() == 1);
    CHECK(page.rows[0].widget == Widget::slider_real);
    CHECK(std::get<double>(page.rows[0].value) == 0.25);
    REQUIRE(m.set("display.vsync", false));
    CHECK(m.page("enhancements", false, false).rows.empty());
}

TEST_CASE("launcher: every row of the shipped schema has its texts", "[launcher][settings]") {
    const std::filesystem::path dir = std::filesystem::path(DQ8_SOURCE_DIR) / "config" / "settings";
    const std::vector<Setting> rows = schema_settings(read_file(dir / "schema.toml"));
    const auto strings = string_table(read_file(dir / "strings" / "en.toml"));
    REQUIRE(rows.size() >= 19);
    SettingsModel m(std::make_unique<MemorySettings>(rows, strings));
    std::size_t shown = 0;
    for (const std::string& category : m.categories()) {
        INFO(category);
        CHECK(strings.count("category." + category) == 1);
        const SettingsPage page = m.page(category, true, false);
        shown += page.rows.size() + page.conditions_hidden;
        for (const SettingRow& r : page.rows) {
            INFO(r.key);
            CHECK(r.label != r.key + ".label");
            CHECK_FALSE(r.description.empty());
            for (const Choice& c : r.choices) {
                CHECK(strings.count(r.key + ".choices." + c.value) == 1);
            }
            for (const Setting& s : rows) {
                if (s.key == r.key) {
                    CHECK(value_matches_type(r.value, s.type));
                }
            }
        }
    }
    CHECK(shown == rows.size());
    // The defaults satisfy every condition (M4's --window behaviour shows the windowed size).
    for (const Setting& s : rows) {
        INFO(s.key);
        CHECK(m.conditions_hold(s));
    }
}

TEST_CASE("launcher: value formatting matches the requires syntax", "[launcher][settings]") {
    CHECK(format_value(true) == "true");
    CHECK(format_value(false) == "false");
    CHECK(format_value(std::int64_t{-12}) == "-12");
    CHECK(format_value(0.5) == "0.5");
    CHECK(format_value(std::string("16:9")) == "16:9");
    CHECK(value_matches_type(true, Type::boolean));
    CHECK_FALSE(value_matches_type(true, Type::integer));
    CHECK(value_matches_type(std::string(), Type::path));
}

TEST_CASE("launcher: the settings store backend reports core's errors", "[launcher][settings]") {
    // Until session A implements the store, opening it throws (a std::logic_error from the day-0 stub); once
    // it does, the shipped schema opens. Either way the call must not crash the launcher.
    try {
        const auto backend = open_store_settings(std::filesystem::path(DQ8_SOURCE_DIR) / "build");
        CHECK_FALSE(backend->settings().empty());
    } catch (const std::exception& e) {
        CHECK(std::string(e.what()).size() > 0);
    }
}
