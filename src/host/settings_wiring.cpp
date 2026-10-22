// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Settings to dq8-game (settings_wiring.h): the store of a run and the settings-to-options table.
#include "host/settings_wiring.h"

#include <utility>

namespace dq8::host {

namespace {

namespace settings = core::settings;

// --- M5 session A: audio output and latency, rumble, movies, saves ---

void apply_audio_output(Options& o, const settings::Store& s) {
    o.audio = s.get_string("audio.output");
}

void apply_audio_latency(Options& o, const settings::Store& s) {
    o.audio_latency_ms = static_cast<std::uint32_t>(s.get_int("audio.latency_ms"));
}

void apply_rumble(Options& o, const settings::Store& s) {
    o.rumble = s.get_bool("input.rumble");
}

void apply_movies(Options& o, const settings::Store& s) {
    o.movies = s.get_string("game.movies");
}

// Empty keeps M4's default: the user data directory with --window, in-memory cards headless.
void apply_save_dir(Options& o, const settings::Store& s) {
    std::string dir = s.get_string("game.save_dir");
    if (!dir.empty()) {
        o.save_dir = std::move(dir);
    }
}

void apply_memory_card2(Options& o, const settings::Store& s) {
    o.memory_card2 = s.get_string("game.memory_card2");
}

} // namespace

const std::vector<SettingOption>& setting_options() {
    static const std::vector<SettingOption> rows{
        {"audio.output", "audio", &apply_audio_output},
        {"audio.latency_ms", "audio-latency", &apply_audio_latency},
        {"input.rumble", "rumble", &apply_rumble},
        {"game.movies", "movies", &apply_movies},
        {"game.save_dir", "save-dir", &apply_save_dir},
        {"game.memory_card2", "memory-card2", &apply_memory_card2},
        // M5 session C: the display, input and audio-device rows go here.
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
        row.apply(o, store);
        applied.push_back(std::string(row.key) + "=" + settings::to_text(store.get(row.key)));
    }
    return applied;
}

} // namespace dq8::host
