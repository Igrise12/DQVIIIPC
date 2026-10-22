// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// M5 session C: the display, input and audio options on the host side (display_wiring.h).
#include "host/display_wiring.h"

#include "core/platform/paths.h"
#include "dq8/core/error.h"
#include "game/display_watcher.h"

#include <exception>
#include <system_error>

namespace dq8::host {

InternalResolution internal_resolution(std::uint32_t factor) noexcept {
    switch (factor) {
    case 2:
        return {4, true};
    case 3:
        return {8, true};
    case 4:
        return {16, true};
    default:
        return {1, false};
    }
}

std::filesystem::path pipeline_cache_file(const Options& o) {
    if (o.pipeline_cache) {
        return *o.pipeline_cache == "none" ? std::filesystem::path()
                                           : std::filesystem::path(*o.pipeline_cache);
    }
    if (o.display != Display::window) {
        return {};
    }
    try {
        return core::platform::user_data_dir() / "cache" / "parallel-gs.pipelines";
    } catch (const core::Error&) {
        return {}; // no home directory: run without a cache
    }
}

std::filesystem::path input_profile_file(const Options& o, const std::filesystem::path& config_dir) {
    const std::string& value = o.input_profile;
    const bool is_file = (value.size() > 5 && value.compare(value.size() - 5, 5, ".toml") == 0) ||
                         value.find('/') != std::string::npos || value.find('\\') != std::string::npos;
    if (is_file) {
        return value;
    }
    if (!core::input::valid_profile_name(value)) {
        throw UsageError("--input-profile '" + value +
                         "': a profile name (letters, digits, '_' and '-') or a .toml file");
    }
    return core::input::profile_file(config_dir, value);
}

core::input::Profile load_input_profile(const Options& o, std::vector<std::string>& warnings) {
    std::filesystem::path config_dir;
    try {
        config_dir = core::platform::user_config_dir();
    } catch (const core::Error&) {
        // No home directory: only an explicit file can be read.
    }
    const std::filesystem::path file = input_profile_file(o, config_dir);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        if (o.input_profile != "default") {
            warnings.push_back("no input profile at " + file.string() + "; using the built-in bindings");
        }
        return core::input::default_profile();
    }
    try {
        return core::input::load_profile(file);
    } catch (const std::exception& e) {
        warnings.push_back(std::string(e.what()) + "; using the built-in bindings");
        return core::input::default_profile();
    }
}

rt::WindowConfig window_config(const Options& o, std::vector<std::string>& warnings) {
    rt::WindowConfig c;
    c.width = o.window_width;
    c.height = o.window_height;
    c.vsync = o.vsync;
    c.mode = o.window_mode == "borderless"   ? rt::WindowMode::borderless
             : o.window_mode == "fullscreen" ? rt::WindowMode::fullscreen
                                             : rt::WindowMode::windowed;
    c.filter = o.filter == "nearest" ? rt::ScaleFilter::nearest : rt::ScaleFilter::linear;
    c.integer_scaling = o.integer_scaling;
    c.input.profile = load_input_profile(o, warnings);
    c.input.deadzone_percent = o.deadzone;
    c.input.rumble_percent = o.rumble_strength;
    return c;
}

double present_aspect(const Options& o, const game::DisplayWatcher* watcher) noexcept {
    if (o.aspect == "16:9") {
        return 16.0 / 9.0;
    }
    if (o.aspect == "stretch") {
        return 0.0;
    }
    if (o.aspect == "auto" && watcher != nullptr && watcher->widescreen()) {
        return 16.0 / 9.0;
    }
    return 4.0 / 3.0;
}

} // namespace dq8::host
