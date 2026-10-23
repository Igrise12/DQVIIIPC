// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "model/environment.h"

#include "core/platform/paths.h"
#include "dq8/core/process.h"

#include <system_error>
#include <vector>

namespace dq8::launcher {

namespace fs = std::filesystem;

namespace {

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// "/opt/dq8/bin/" (SDL_GetBasePath's form) as "/opt/dq8/bin", so parent_path() is the parent directory.
fs::path directory(const fs::path& dir) {
    return !dir.empty() && !dir.has_filename() ? dir.parent_path() : dir;
}

} // namespace

std::string to_utf8(const fs::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

fs::path from_utf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::optional<fs::path> locate_program(const fs::path& executable_dir_in, std::string_view name,
                                       std::string_view build_subdir) {
    const fs::path executable_dir = directory(executable_dir_in);
#ifdef _WIN32
    const std::string file = std::string(name) + ".exe";
#else
    const std::string file(name);
#endif
    if (!executable_dir.empty()) {
        for (const fs::path& candidate :
             {executable_dir / file, executable_dir.parent_path() / std::string(build_subdir) / file}) {
            if (is_file(candidate)) {
                return candidate;
            }
        }
    }
    return core::find_program(fs::path(file));
}

std::optional<fs::path> find_shared_file(const fs::path& executable_dir, const fs::path& in_tree,
                                         const fs::path& installed) {
    std::vector<fs::path> candidates;
    fs::path dir = directory(executable_dir);
    for (int up = 0; up < 8 && !dir.empty(); ++up) { // 8: a multi-config build adds Release/ or Debug/
        candidates.push_back(dir / in_tree);
        candidates.push_back(dir / installed);
        const fs::path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    candidates.push_back(in_tree); // the current directory last: the program's own files come first
    for (const fs::path& c : candidates) {
        if (is_file(c)) {
            return c;
        }
    }
    return std::nullopt;
}

Environment discover_environment(const fs::path& executable_dir, const fs::path& game, const fs::path& ctl) {
    Environment env;
    env.executable_dir = directory(executable_dir);
    env.game = !game.empty() ? game : locate_program(executable_dir, "dq8-game", "host").value_or(fs::path());
    env.ctl = !ctl.empty() ? ctl : locate_program(executable_dir, "dq8ctl", "cli").value_or(fs::path());
    try {
        env.config_dir = core::platform::user_config_dir();
        env.state_file = env.config_dir / "launcher.json";
    } catch (...) {
        // No home directory: nothing is remembered between runs.
    }
    try {
        env.data_dir = core::platform::user_data_dir();
    } catch (...) {
    }
    return env;
}

} // namespace dq8::launcher
