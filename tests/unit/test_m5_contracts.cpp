// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// M5 day-0 contracts (Docs/milestones/M5-launcher.md): the error texts, the settings schema and its strings,
// the setup's event line, the user configuration directory, the registry's lookup and dq8-game's new options.
// The sessions' own tests live in the m5_*_tests.cmake lists.
#include "core/platform/paths.h"
#include "dq8/core/error.h"
#include "dq8/core/error_text.h"
#include "dq8/core/registry.h"
#include "dq8/core/settings.h"
#include "dq8/core/setup.h"
#include "dq8/core/toolchain.h"
#include "host/options.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <toml.hpp>
#include <vector>

namespace {

namespace fs = std::filesystem;
using dq8::core::ErrorCode;

bool is_setting_key(std::string_view key) {
    const std::size_t dot = key.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == key.size() ||
        key.find('.', dot + 1) != std::string_view::npos) {
        return false;
    }
    for (const char c : key) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) {
            return false;
        }
    }
    return true;
}

dq8::host::Options parse(std::initializer_list<const char*> args) {
    std::vector<const char*> argv{"dq8-game"};
    argv.insert(argv.end(), args.begin(), args.end());
    return dq8::host::parse_options(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST_CASE("m5 contracts: every error code has a stable name and a message with an action", "[m5][errors]") {
    std::set<std::string_view> names;
    // registry_parse is the last code; a new code without a text fails to compile (-Wswitch in
    // error_text.cpp).
    for (int i = 0; i <= static_cast<int>(ErrorCode::registry_parse); ++i) {
        const auto code = static_cast<ErrorCode>(i);
        const std::string_view name = dq8::core::to_string(code);
        INFO("code " << i << " " << name);
        CHECK(name != "unknown");
        CHECK(names.insert(name).second);
        const dq8::core::ErrorText text = dq8::core::error_text(code);
        CHECK_FALSE(text.message.empty());
        CHECK_FALSE(text.action.empty());
        CHECK(text.message.back() == '.');
        CHECK(text.action.back() == '.');
    }
    CHECK(dq8::core::to_string(ErrorCode::settings_invalid) == "settings-invalid");
    CHECK(dq8::core::to_string(ErrorCode::toolchain_incomplete) == "toolchain-incomplete");
    CHECK(dq8::core::to_string(ErrorCode::setup_cancelled) == "setup-cancelled");
    CHECK_FALSE(dq8::core::is_mismatch(ErrorCode::maps_missing));
}

TEST_CASE("m5 contracts: the settings schema is well formed and every text exists", "[m5][settings]") {
    const fs::path dir = fs::path(DQ8_SOURCE_DIR) / "config" / "settings";
    const toml::value schema = toml::parse(dir / "schema.toml");
    const toml::value strings_file = toml::parse(dir / "strings" / "en.toml");
    // Copies, not references: GCC 13+ flags references into toml::find results (-Wdangling-reference).
    const auto strings = toml::find<toml::table>(strings_file, "strings");
    const auto has_text = [&](const std::string& id) { return strings.count(id) == 1; };

    CHECK(toml::find<int>(schema, "schema_version") == 1);
    const std::set<std::string> types{"boolean", "integer", "real", "choice", "text", "path"};
    std::map<std::string, toml::value> settings;
    const auto rows = toml::find<toml::array>(schema, "setting");
    for (const toml::value& s : rows) {
        const auto key = toml::find<std::string>(s, "key");
        INFO(key);
        CHECK(is_setting_key(key));
        CHECK(settings.emplace(key, s).second);
        const auto type = toml::find<std::string>(s, "type");
        CHECK(types.count(type) == 1);
        // The texts are string-table ids, never inline text.
        CHECK(toml::find<std::string>(s, "label") == key + ".label");
        CHECK(toml::find<std::string>(s, "description") == key + ".description");
        CHECK(has_text(key + ".label"));
        CHECK(has_text(key + ".description"));
        CHECK(has_text("category." + toml::find<std::string>(s, "category")));
        CHECK(s.contains("owner"));

        const toml::value def = toml::find(s, "default");
        if (type == "boolean") {
            CHECK(def.is_boolean());
        } else if (type == "integer") {
            REQUIRE(def.is_integer());
            CHECK(toml::find<long long>(s, "min") <= def.as_integer());
            CHECK(def.as_integer() <= toml::find<long long>(s, "max"));
        } else if (type == "real") {
            REQUIRE(def.is_floating());
            CHECK(toml::find<double>(s, "min") <= def.as_floating());
            CHECK(def.as_floating() <= toml::find<double>(s, "max"));
        } else if (type == "choice") {
            const auto choices = toml::find<std::vector<std::string>>(s, "choices");
            REQUIRE_FALSE(choices.empty());
            CHECK(std::set<std::string>(choices.begin(), choices.end()).count(def.as_string()) == 1);
            for (const std::string& choice : choices) {
                CHECK(has_text(key + ".choices." + choice));
            }
        } else {
            CHECK(def.is_string());
        }
    }
    REQUIRE(settings.size() >= 19);

    // requires = ["key=value", ...] names other settings and values they can take.
    for (const auto& [key, s] : settings) {
        if (!s.contains("requires")) {
            continue;
        }
        for (const std::string& condition : toml::find<std::vector<std::string>>(s, "requires")) {
            INFO(key << " requires " << condition);
            const std::size_t eq = condition.find('=');
            REQUIRE(eq != std::string::npos);
            const auto other = settings.find(condition.substr(0, eq));
            REQUIRE(other != settings.end());
            CHECK(other->first != key);
            const std::string value = condition.substr(eq + 1);
            const auto other_type = toml::find<std::string>(other->second, "type");
            if (other_type == "boolean") {
                CHECK((value == "true" || value == "false"));
            } else if (other_type == "choice") {
                const auto choices = toml::find<std::vector<std::string>>(other->second, "choices");
                CHECK(std::set<std::string>(choices.begin(), choices.end()).count(value) == 1);
            }
        }
    }

    // The keys the M5 sessions wire up (the settings-to-options table of the milestone record).
    for (const char* key : {"display.window_mode", "display.width", "display.height", "display.vsync",
                            "display.internal_resolution", "display.aspect", "display.integer_scaling",
                            "display.filter", "audio.output", "audio.device", "audio.volume",
                            "audio.latency_ms", "input.profile", "input.deadzone", "input.rumble",
                            "input.rumble_strength", "game.movies", "game.save_dir", "game.memory_card2"}) {
        INFO(key);
        CHECK(settings.count(key) == 1);
    }
    // Defaults reproduce M4's behaviour, so default settings move no golden hash.
    CHECK(toml::find<std::string>(settings.at("game.movies"), "default") == "auto");
    CHECK(toml::find<std::string>(settings.at("display.internal_resolution"), "default") == "1x");
    CHECK(toml::find<std::string>(settings.at("game.memory_card2"), "default") == "none");
}

TEST_CASE("m5 contracts: the setup's stage events are one JSON line each", "[m5][setup]") {
    namespace setup = dq8::core::setup;
    setup::Event start;
    CHECK(setup::to_json_line(start) ==
          R"({"ev":"stage","stage":"verify","state":"start","done":0,"total":0})");

    setup::Event e;
    e.stage = setup::Stage::build;
    e.state = setup::State::error;
    e.done = 120;
    e.total = 845;
    e.unit = "main";
    e.message = "a \"quoted\"\tpath\\x\n";
    e.code = ErrorCode::module_build_failed;
    CHECK(setup::to_json_line(e) ==
          R"({"ev":"stage","stage":"build","state":"error","done":120,"total":845,"unit":"main",)"
          R"("message":"a \"quoted\"\tpath\\x\n","code":"module-build-failed"})");

    std::set<std::string_view> stages;
    for (const setup::Stage stage :
         {setup::Stage::verify, setup::Stage::extract, setup::Stage::toolchain, setup::Stage::maps,
          setup::Stage::prepare, setup::Stage::generate, setup::Stage::build, setup::Stage::smoke,
          setup::Stage::register_install}) {
        CHECK(stages.insert(setup::to_string(stage)).second);
    }
    CHECK(setup::to_string(setup::Stage::register_install) == "register");
    CHECK(setup::to_string(setup::State::skipped) == "skipped");
}

TEST_CASE("m5 contracts: the user configuration directory follows the platform's convention", "[m5][paths]") {
    namespace platform = dq8::core::platform;
    static std::string xdg_config;
    static std::string home;
    static std::string appdata;
    const auto env = [](std::string_view name) -> std::string_view {
        if (name == "XDG_CONFIG_HOME") {
            return xdg_config;
        }
        if (name == "HOME") {
            return home;
        }
        if (name == "APPDATA") {
            return appdata;
        }
        return {};
    };
    xdg_config.clear();
    home.clear();
    appdata.clear();
    CHECK_THROWS(platform::user_config_dir_from(env));
#if defined(_WIN32)
    appdata = "C:\\Users\\u\\AppData\\Roaming";
    CHECK(platform::user_config_dir_from(env) == platform::user_data_dir_from(env));
#elif defined(__APPLE__)
    home = "/Users/u";
    CHECK(platform::user_config_dir_from(env) == platform::user_data_dir_from(env));
#else
    home = "/home/u";
    CHECK(platform::user_config_dir_from(env) == fs::path("/home/u/.config/dq8-native"));
    xdg_config = "relative/ignored";
    CHECK(platform::user_config_dir_from(env) == fs::path("/home/u/.config/dq8-native"));
    xdg_config = "/cfg";
    CHECK(platform::user_config_dir_from(env) == fs::path("/cfg/dq8-native"));
#endif
}

TEST_CASE("m5 contracts: the registry keeps one install per profile", "[m5][registry]") {
    dq8::core::registry::Registry registry;
    CHECK(registry.find("SLUS_21207") == nullptr);
    registry.upsert({"SLUS_21207", "a.iso", "mods-a", "11", "2026-10-22T00:00:00Z", "0.0.1"});
    registry.upsert({"OTHER_00000", "b.iso", "mods-b", "22", "2026-10-22T00:00:00Z", "0.0.1"});
    registry.upsert({"SLUS_21207", "c.iso", "mods-c", "33", "2026-10-22T01:00:00Z", "0.0.1"});
    REQUIRE(registry.installs().size() == 2);
    const auto* install = registry.find("SLUS_21207");
    REQUIRE(install != nullptr);
    CHECK(install->module_dir == fs::path("mods-c"));
    CHECK(install->manifest_sha1 == "33");
    CHECK(dq8::core::registry::default_file().filename() == "installs.toml");
    CHECK(dq8::core::toolchain::default_jobs() >= 1);
}

TEST_CASE("m5 contracts: dq8-game parses --config, --set and --status", "[m5][host]") {
    const dq8::host::Options o =
        parse({"--elf", "a.elf", "--module-dir", "m", "--config", "user.toml", "--set", "display.vsync=false",
               "--set", "game.save_dir=C:/x=y", "--status", "-"});
    REQUIRE(o.config);
    CHECK(*o.config == fs::path("user.toml"));
    CHECK(o.sets == std::vector<std::string>{"display.vsync=false", "game.save_dir=C:/x=y"});
    REQUIRE(o.status);
    CHECK(*o.status == "-");

    const dq8::host::Options none = parse({"--elf", "a.elf", "--module-dir", "m"});
    CHECK_FALSE(none.config);
    CHECK(none.sets.empty());
    CHECK_FALSE(none.status);

    CHECK_THROWS_AS(parse({"--elf", "a.elf", "--module-dir", "m", "--set", "display.vsync"}),
                    dq8::host::UsageError);
    CHECK_THROWS_AS(parse({"--elf", "a.elf", "--module-dir", "m", "--set", "=1"}), dq8::host::UsageError);
}

TEST_CASE("m5 contracts: shipped data is found from a multi-config build's output directory", "[m5][paths]") {
    // Visual Studio and Ninja Multi-Config put programs one directory deeper (<target dir>/Release); the
    // searches still reach the source tree (seen on the MSVC CI job).
    const fs::path deeper = fs::path(DQ8_CONTRACT_MODULE_DIR) / "Release";
    const fs::path settings = dq8::core::settings::find_settings_dir(deeper);
    REQUIRE_FALSE(settings.empty());
    CHECK(fs::equivalent(settings, fs::path(DQ8_SOURCE_DIR) / "config" / "settings"));
    CHECK(fs::equivalent(
        dq8::core::platform::find_data_dir(deeper, {fs::path("data") / "regions"}, "SLUS_21207.toml"),
        fs::path(DQ8_SOURCE_DIR) / "data" / "regions"));
}
