// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
#pragma once

#include <string>
#include <string_view>
#include <vector>

// Memory-card path handling as mcman sees paths: '/' or '\\' separators, an optional "mcN:" device prefix,
// "." and ".." components, absolute paths or paths relative to the card's current directory; and the '*'
// and '?' wildcards of GetDir.
namespace dq8::rt::mc {

// The components of `path` resolved against `current` (absolute, components only): empty components and "."
// are dropped, ".." pops (never past the root).
std::vector<std::string> resolve_path(std::string_view path, const std::vector<std::string>& current);
// "/A/B" for {"A", "B"}, "/" for none.
std::string join_path(const std::vector<std::string>& components);
// '*' matches any run of characters (also none), '?' any one character, or nothing once the name has ended
// (so "dq8???" matches "dq8_1"); everything else matches itself, case-sensitively.
bool wildcard_match(std::string_view pattern, std::string_view name);
bool has_wildcard(std::string_view name);

} // namespace dq8::rt::mc
