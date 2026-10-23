// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Where the launcher finds the programs it starts and the files it shows, in an install (bin/ beside share/)
// and in a build tree (src/launcher/ beside src/host/ and src/cli/).
namespace dq8::launcher {

struct Environment {
    std::filesystem::path executable_dir;
    std::filesystem::path game;       // dq8-game; empty when not found
    std::filesystem::path ctl;        // dq8ctl; empty when not found
    std::filesystem::path config_dir; // platform::user_config_dir(); empty when the environment names no home
    std::filesystem::path data_dir;   // platform::user_data_dir(); likewise
    std::filesystem::path state_file; // the launcher's own state, config_dir / "launcher.json"
};

// Paths as UTF-8 text and back (process arguments, JSON, SDL's dialogs), the same on every platform.
std::string to_utf8(const std::filesystem::path& path);
std::filesystem::path from_utf8(std::string_view text);

// `game` and `ctl` are explicit choices (--game, --dq8ctl); empty ones are looked for.
Environment discover_environment(const std::filesystem::path& executable_dir,
                                 const std::filesystem::path& game = {},
                                 const std::filesystem::path& ctl = {});

// `name` (plus ".exe" on Windows) in `executable_dir`, then in its sibling `build_subdir` (the build tree),
// then on PATH.
std::optional<std::filesystem::path> locate_program(const std::filesystem::path& executable_dir,
                                                    std::string_view name, std::string_view build_subdir);

// A file shipped with the program: `in_tree` (relative to the source tree) under an ancestor of
// `executable_dir`, or `installed` (relative to the install prefix) beside it, else `in_tree` under the
// current directory.
std::optional<std::filesystem::path> find_shared_file(const std::filesystem::path& executable_dir,
                                                      const std::filesystem::path& in_tree,
                                                      const std::filesystem::path& installed);

} // namespace dq8::launcher
