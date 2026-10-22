// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "platform/paths.h"

#include "dq8/core/error.h"

#include <cstdlib>
#include <string>

namespace dq8::core::platform {

namespace fs = std::filesystem;

namespace {

std::string_view environment(std::string_view name) {
    const char* value = std::getenv(std::string(name).c_str());
    return value != nullptr ? std::string_view(value) : std::string_view();
}

} // namespace

fs::path user_data_dir_from(std::string_view (*get)(std::string_view name)) {
#if defined(_WIN32)
    if (const std::string_view appdata = get("APPDATA"); !appdata.empty()) {
        return fs::path(std::string(appdata)) / "DQ8-Native";
    }
    if (const std::string_view profile = get("USERPROFILE"); !profile.empty()) {
        return fs::path(std::string(profile)) / "AppData" / "Roaming" / "DQ8-Native";
    }
#elif defined(__APPLE__)
    if (const std::string_view home = get("HOME"); !home.empty()) {
        return fs::path(std::string(home)) / "Library" / "Application Support" / "DQ8-Native";
    }
#else
    // XDG: a relative XDG_DATA_HOME is invalid and ignored.
    if (const std::string_view xdg = get("XDG_DATA_HOME"); !xdg.empty() && xdg.front() == '/') {
        return fs::path(std::string(xdg)) / "dq8-native";
    }
    if (const std::string_view home = get("HOME"); !home.empty()) {
        return fs::path(std::string(home)) / ".local" / "share" / "dq8-native";
    }
#endif
    throw Error(ErrorCode::io_open, "no home directory is set, so there is no user data directory");
}

fs::path user_data_dir() {
    return user_data_dir_from(&environment);
}

} // namespace dq8::core::platform
