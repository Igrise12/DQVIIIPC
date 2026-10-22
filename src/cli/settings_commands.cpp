// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// dq8ctl settings list|get|set|reset (M5): the user settings file through dq8/core/settings.h, the same store
// the launcher and dq8-game --config use.
#include "cli.h"
#include "dq8/core/settings.h"

#include <cstdio>
#include <optional>
#include <variant>

namespace dq8::cli {

namespace {

namespace settings = dq8::core::settings;

void json_value(Json& j, const settings::Value& v) {
    if (const bool* b = std::get_if<bool>(&v)) {
        j.value(*b);
    } else if (const std::int64_t* i = std::get_if<std::int64_t>(&v)) {
        j.value(*i);
    } else if (std::holds_alternative<double>(v)) {
        j.value(settings::to_text(v)); // the writer has no reals; the text form reads back exactly
    } else {
        j.value(std::get<std::string>(v));
    }
}

} // namespace

int run_settings(const Args& args, const char* argv0) {
    const bool json = args.flags.count("json") != 0;
    if (args.positional.empty()) {
        std::fputs("dq8ctl settings: expected list, get KEY, set KEY=VALUE..., reset [KEY...] or defaults\n",
                   stderr);
        return usage(stderr);
    }
    const std::string action = args.positional[0];
    const std::vector<std::string> operands(args.positional.begin() + 1, args.positional.end());
    const fs::path exe_dir = executable_dir(argv0);
    const fs::path dir = settings::find_settings_dir(exe_dir);
    const auto config = args.values.find("config");
    const fs::path file =
        config != args.values.end() ? fs::path(config->second) : settings::default_user_file(exe_dir);
    const auto profile = args.values.find("profile");
    const settings::Layer layer =
        profile != args.values.end() ? settings::Layer::profile : settings::Layer::user;

    settings::Store store(settings::Schema::load(dir / "schema.toml"));
    if (profile != args.values.end()) {
        store.use_profile(profile->second);
    }
    store.load(settings::Layer::user, file);
    for (const std::string& w : store.warnings()) {
        std::fprintf(stderr, "warning: %s\n", w.c_str());
    }

    if (action == "list" || action == "get") {
        if ((action == "get") != (operands.size() == 1) || (action == "list" && !operands.empty())) {
            std::fputs("dq8ctl settings: get takes one KEY, list none\n", stderr);
            return usage(stderr);
        }
        if (action == "get") {
            const settings::Value v = store.get(operands[0]);
            if (json) {
                Json j;
                j.begin_object();
                j.member("ok", true);
                j.member("key", operands[0]);
                j.key("value");
                json_value(j, v);
                j.member("source", settings::to_string(store.source(operands[0])));
                j.end_object();
                std::printf("%s\n", j.str().c_str());
            } else {
                std::printf("%s\n", settings::to_text(v).c_str());
            }
            return exit_ok;
        }
        std::optional<settings::StringTable> strings;
        try {
            strings = settings::StringTable::load(dir / "strings" / "en.toml");
        } catch (const Error&) {
            // Labels are a convenience here; the keys still list.
        }
        const auto text = [&](const std::string& id) { return strings ? std::string(strings->get(id)) : id; };
        Json j;
        j.begin_object();
        j.member("ok", true);
        j.member("file", path_text(file));
        j.key("settings");
        j.begin_array();
        for (const settings::Setting& s : store.schema().settings()) {
            const settings::Value v = store.get(s.key);
            const std::string_view source = settings::to_string(store.source(s.key));
            if (!json) {
                std::printf("%-28s = %-12s (%s)%s  %s; %s\n", s.key.c_str(), settings::to_text(v).c_str(),
                            std::string(source).c_str(), store.applies(s.key) ? "" : " [not applied]",
                            text(s.label).c_str(), settings::describe_allowed(s).c_str());
                continue;
            }
            j.begin_object();
            j.member("key", s.key);
            j.key("value");
            json_value(j, v);
            j.member("source", source);
            j.member("type", settings::to_string(s.type));
            j.key("default");
            json_value(j, s.default_value);
            j.member("allowed", settings::describe_allowed(s));
            j.member("category", s.category);
            j.member("label", text(s.label));
            j.member("description", text(s.description));
            j.member("advanced", s.advanced);
            j.member("requires_restart", s.requires_restart);
            j.member("applies", store.applies(s.key));
            j.end_object();
        }
        j.end_array();
        j.end_object();
        if (json) {
            std::printf("%s\n", j.str().c_str());
        } else {
            std::printf("(file: %s)\n", path_text(file).c_str());
        }
        return exit_ok;
    }

    if (action == "set") {
        // KEY=VALUE..., or the two-word form KEY VALUE.
        std::vector<std::string> assignments = operands;
        if (operands.size() == 2 && operands[0].find('=') == std::string::npos) {
            assignments = {operands[0] + "=" + operands[1]};
        }
        if (assignments.empty()) {
            std::fputs("dq8ctl settings set: expected KEY=VALUE\n", stderr);
            return usage(stderr);
        }
        for (const std::string& a : assignments) {
            const std::size_t eq = a.find('=');
            if (eq == std::string::npos || eq == 0) {
                throw Error(ErrorCode::settings_invalid, "expected KEY=VALUE, not '" + a + "'");
            }
            const settings::Setting* s = store.schema().find(a.substr(0, eq));
            if (s == nullptr) {
                throw Error(ErrorCode::settings_invalid, "unknown setting '" + a.substr(0, eq) + "'");
            }
            store.set(layer, s->key, settings::parse_value(*s, a.substr(eq + 1)));
        }
    } else if (action == "reset") {
        if (operands.empty()) {
            store.clear(layer);
        }
        for (const std::string& key : operands) {
            store.reset(layer, key);
        }
    } else if (action == "defaults") {
        // A complete file with every setting at its default, to start editing from (replaces the file).
        if (!operands.empty() || layer != settings::Layer::user) {
            std::fputs("dq8ctl settings defaults: no keys and no --profile\n", stderr);
            return usage(stderr);
        }
        settings::Store defaults(store.schema());
        for (const settings::Setting& s : store.schema().settings()) {
            defaults.set(settings::Layer::user, s.key, s.default_value);
        }
        defaults.save(settings::Layer::user, file);
        std::printf("wrote %s\n", path_text(file).c_str());
        return exit_ok;
    } else {
        std::fprintf(stderr, "dq8ctl settings: unknown action '%s'\n", action.c_str());
        return usage(stderr);
    }
    store.save(settings::Layer::user, file);
    if (json) {
        Json j;
        j.begin_object();
        j.member("ok", true);
        j.member("file", path_text(file));
        j.end_object();
        std::printf("%s\n", j.str().c_str());
    } else {
        std::printf("wrote %s\n", path_text(file).c_str());
    }
    return exit_ok;
}

} // namespace dq8::cli
