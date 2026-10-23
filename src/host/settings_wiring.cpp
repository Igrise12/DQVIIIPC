// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Settings to dq8-game (settings_wiring.h): the store of a run and the settings-to-options table.
#include "host/settings_wiring.h"

#include <cstdint>
#include <string>
#include <utility>

namespace dq8::host {

namespace {

namespace settings = core::settings;

// --- M5 session A: audio output and latency, rumble, movies, saves ---

bool apply_audio_output(Options& o, const settings::Store& s) {
    o.audio = s.get_string("audio.output");
    return true;
}

bool apply_audio_latency(Options& o, const settings::Store& s) {
    o.audio_latency_ms = static_cast<std::uint32_t>(s.get_int("audio.latency_ms"));
    return true;
}

bool apply_rumble(Options& o, const settings::Store& s) {
    o.rumble = s.get_bool("input.rumble");
    return true;
}

bool apply_movies(Options& o, const settings::Store& s) {
    o.movies = s.get_string("game.movies");
    return true;
}

// Empty keeps M4's default: the user data directory with --window, in-memory cards headless.
bool apply_save_dir(Options& o, const settings::Store& s) {
    std::string dir = s.get_string("game.save_dir");
    if (!dir.empty()) {
        o.save_dir = std::move(dir);
    }
    return true;
}

bool apply_memory_card2(Options& o, const settings::Store& s) {
    o.memory_card2 = s.get_string("game.memory_card2");
    return true;
}

// --- M5 session C: display, input and the audio device (the schema validated every value already) ---

bool apply_window_mode(Options& o, const settings::Store& s) {
    o.window_mode = s.get_string("display.window_mode");
    return true;
}

bool apply_width(Options& o, const settings::Store& s) {
    o.window_width = static_cast<int>(s.get_int("display.width"));
    return true;
}

bool apply_height(Options& o, const settings::Store& s) {
    o.window_height = static_cast<int>(s.get_int("display.height"));
    return true;
}

bool apply_vsync(Options& o, const settings::Store& s) {
    o.vsync = s.get_bool("display.vsync");
    return true;
}

// "1x".."4x". Only the paraLLEl-GS backend renders at a higher resolution, and an explicit --ssaa keeps its
// own rate (the flags refuse --ssaa together with --internal-resolution).
bool apply_internal_resolution(Options& o, const settings::Store& s) {
    if (o.gs != GsKind::parallel || o.was_given("ssaa")) {
        return false;
    }
    const std::string value = s.get_string("display.internal_resolution");
    o.internal_resolution = static_cast<std::uint32_t>(value.at(0) - '0');
    return true;
}

bool apply_aspect(Options& o, const settings::Store& s) {
    o.aspect = s.get_string("display.aspect");
    return true;
}

bool apply_integer_scaling(Options& o, const settings::Store& s) {
    o.integer_scaling = s.get_bool("display.integer_scaling");
    return true;
}

bool apply_filter(Options& o, const settings::Store& s) {
    o.filter = s.get_string("display.filter");
    return true;
}

bool apply_audio_device(Options& o, const settings::Store& s) {
    o.audio_device = s.get_string("audio.device");
    return true;
}

bool apply_volume(Options& o, const settings::Store& s) {
    o.volume = static_cast<std::uint32_t>(s.get_int("audio.volume"));
    return true;
}

bool apply_input_profile(Options& o, const settings::Store& s) {
    o.input_profile = s.get_string("input.profile");
    return true;
}

bool apply_deadzone(Options& o, const settings::Store& s) {
    o.deadzone = static_cast<std::uint32_t>(s.get_int("input.deadzone"));
    return true;
}

bool apply_rumble_strength(Options& o, const settings::Store& s) {
    o.rumble_strength = static_cast<std::uint32_t>(s.get_int("input.rumble_strength"));
    return true;
}

} // namespace

const std::vector<SettingOption>& setting_options() {
    static const std::vector<SettingOption> rows{
        {"display.window_mode", "window-mode", &apply_window_mode},
        {"display.width", "window-size", &apply_width},
        {"display.height", "window-size", &apply_height},
        {"display.vsync", "vsync", &apply_vsync},
        {"display.internal_resolution", "internal-resolution", &apply_internal_resolution},
        {"display.aspect", "aspect", &apply_aspect},
        {"display.integer_scaling", "integer-scaling", &apply_integer_scaling},
        {"display.filter", "filter", &apply_filter},
        {"audio.output", "audio", &apply_audio_output},
        {"audio.device", "audio-device", &apply_audio_device},
        {"audio.volume", "volume", &apply_volume},
        {"audio.latency_ms", "audio-latency", &apply_audio_latency},
        {"input.profile", "input-profile", &apply_input_profile},
        {"input.deadzone", "deadzone", &apply_deadzone},
        {"input.rumble", "rumble", &apply_rumble},
        {"input.rumble_strength", "rumble-strength", &apply_rumble_strength},
        {"game.movies", "movies", &apply_movies},
        {"game.save_dir", "save-dir", &apply_save_dir},
        {"game.memory_card2", "memory-card2", &apply_memory_card2},
    };
    return rows;
}

std::optional<settings::Store> load_settings(const Options& o, const fs::path& executable_dir) {
    if (!o.config && o.sets.empty()) {
        return std::nullopt;
    }
    const fs::path dir = settings::find_settings_dir(executable_dir);
    settings::Store store(settings::Schema::load(dir / "schema.toml"));
    if (o.config) {
        store.load(settings::Layer::user, *o.config);
    }
    for (const std::string& assignment : o.sets) {
        store.set_from_text(assignment);
    }
    return store;
}

std::vector<std::string> apply_settings(Options& o, const settings::Store& store) {
    std::vector<std::string> applied;
    for (const SettingOption& row : setting_options()) {
        if (o.was_given(row.flag) || !store.applies(row.key)) {
            continue;
        }
        if (!row.apply(o, store)) {
            continue;
        }
        applied.push_back(std::string(row.key) + "=" + settings::to_text(store.get(row.key)));
    }
    return applied;
}

} // namespace dq8::host
