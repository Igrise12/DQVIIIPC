// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "platform/paths.h"

#include "dq8/core/error.h"

#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

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

fs::path user_config_dir_from(std::string_view (*get)(std::string_view name)) {
#if defined(_WIN32) || defined(__APPLE__)
    return user_data_dir_from(get);
#else
    // XDG: a relative XDG_CONFIG_HOME is invalid and ignored.
    if (const std::string_view xdg = get("XDG_CONFIG_HOME"); !xdg.empty() && xdg.front() == '/') {
        return fs::path(std::string(xdg)) / "dq8-native";
    }
    if (const std::string_view home = get("HOME"); !home.empty()) {
        return fs::path(std::string(home)) / ".config" / "dq8-native";
    }
    throw Error(ErrorCode::io_open, "no home directory is set, so there is no user configuration directory");
#endif
}

fs::path user_config_dir() {
    return user_config_dir_from(&environment);
}

fs::path find_data_dir(const fs::path& executable_dir, std::initializer_list<fs::path> relatives,
                       const fs::path& marker) {
    std::vector<fs::path> bases{fs::path()};
    fs::path dir = executable_dir;
    for (int up = 0; up < 8 && !dir.empty(); ++up) { // 8: a multi-config build adds Release/ or Debug/
        bases.push_back(dir);
        if (dir == dir.parent_path()) {
            break;
        }
        dir = dir.parent_path();
    }
    std::error_code ec;
    for (const fs::path& base : bases) {
        for (const fs::path& relative : relatives) {
            const fs::path candidate = base.empty() ? relative : base / relative;
            if (fs::exists(candidate / marker, ec)) {
                return candidate;
            }
        }
    }
    return {};
}

fs::path executable_dir_of(const char* argv0) {
    std::error_code ec;
#if defined(__linux__)
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return self.parent_path();
    }
#endif
    if (argv0 == nullptr) {
        return {};
    }
    const fs::path p = fs::canonical(argv0, ec);
    return ec ? fs::path() : p.parent_path();
}

} // namespace dq8::core::platform
