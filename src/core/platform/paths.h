// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <filesystem>
#include <string_view>

// Where DQ8-Native keeps per-user data (memory cards first; ADR-0016) and settings (ADR-0006).
namespace dq8::core::platform {

// The per-user data directory: %APPDATA%\DQ8-Native on Windows, ~/Library/Application Support/DQ8-Native on
// macOS, $XDG_DATA_HOME/dq8-native (default ~/.local/share/dq8-native) elsewhere. Not created here. Throws
// Error(io_open) when the environment names no home directory.
std::filesystem::path user_data_dir();

// The same rule over explicit environment values (tests): `get` returns a variable's value, empty when unset.
std::filesystem::path user_data_dir_from(std::string_view (*get)(std::string_view name));

// The per-user configuration directory (settings, controller profiles; ADR-0006): $XDG_CONFIG_HOME/dq8-native
// (default ~/.config/dq8-native) on Linux and other Unix systems; the user data directory on Windows and
// macOS. Not created here. Throws Error(io_open) when the environment names no home directory.
std::filesystem::path user_config_dir();
std::filesystem::path user_config_dir_from(std::string_view (*get)(std::string_view name));

} // namespace dq8::core::platform
