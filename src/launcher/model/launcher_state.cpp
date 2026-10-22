// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "model/launcher_state.h"

#include "core/platform/files.h"
#include "model/environment.h"

#include <nlohmann/json.hpp>

namespace dq8::launcher {

namespace {

using json = nlohmann::json;

} // namespace

LauncherState parse_launcher_state(std::string_view text) {
    LauncherState state;
    const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        return state;
    }
    if (const auto it = j.find("last_image"); it != j.end() && it->is_string()) {
        state.last_image = from_utf8(it->get<std::string>());
    }
    if (const auto it = j.find("screen"); it != j.end() && it->is_string()) {
        state.screen = it->get<std::string>();
    }
    if (const auto it = j.find("show_advanced"); it != j.end() && it->is_boolean()) {
        state.show_advanced = it->get<bool>();
    }
    return state;
}

std::string format_launcher_state(const LauncherState& state) {
    json j = json::object();
    j["last_image"] = to_utf8(state.last_image);
    j["screen"] = state.screen;
    j["show_advanced"] = state.show_advanced;
    return j.dump(2) + "\n";
}

LauncherState load_launcher_state(const std::filesystem::path& file) {
    if (file.empty()) {
        return {};
    }
    try {
        return parse_launcher_state(core::platform::read_text_file(file, 1u << 20));
    } catch (...) {
        return {};
    }
}

void save_launcher_state(const std::filesystem::path& file, const LauncherState& state) {
    core::platform::write_file_if_changed(file, format_launcher_state(state));
}

} // namespace dq8::launcher
