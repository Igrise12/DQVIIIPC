// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// What the launcher remembers between runs that is not a setting: the disc image of an unfinished setup (so
// the wizard can resume it) and the page it showed last. A small JSON file in the user configuration
// directory; never game data.
namespace dq8::launcher {

struct LauncherState {
    std::filesystem::path last_image; // the image the last setup was started with
    std::string screen;               // "play", "settings", ...
    bool show_advanced = false;       // the settings pages' advanced toggle
    friend bool operator==(const LauncherState&, const LauncherState&) = default;
};

// A missing or unreadable file gives the defaults: the state is a convenience, never an error.
LauncherState load_launcher_state(const std::filesystem::path& file);
std::string format_launcher_state(const LauncherState& state);
LauncherState parse_launcher_state(std::string_view text);
// Writes atomically; throws core::Error(io_write).
void save_launcher_state(const std::filesystem::path& file, const LauncherState& state);

} // namespace dq8::launcher
