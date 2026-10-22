// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
//
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xRuntime/src/lib/Kernel/Stubs/MemoryCard.cpp (splitMcPathComponents, joinMcPathComponents,
// normalizeGuestMcPathLocked, wildcardMatch). Changes: no host paths and no global card state; the current
// directory is passed in; the device prefix check accepts any "mcN:"; a '?' left over after the name ends
// matches nothing (the game lists "dq8_1" with the pattern "dq8???").
#include "runtime/io/memcard/mc_path.h"

#include <cstddef>

namespace dq8::rt::mc {

namespace {

std::vector<std::string> split_components(std::string_view value) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : value) {
        if (c == '/' || c == '\\') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

} // namespace

std::vector<std::string> resolve_path(std::string_view path, const std::vector<std::string>& current) {
    if (path.size() >= 4 && (path[0] == 'm' || path[0] == 'M') && (path[1] == 'c' || path[1] == 'C') &&
        path[2] >= '0' && path[2] <= '9' && path[3] == ':') {
        path.remove_prefix(4);
    }
    const bool absolute = !path.empty() && (path.front() == '/' || path.front() == '\\');
    std::vector<std::string> parts = absolute ? std::vector<std::string>{} : current;
    for (std::string& part : split_components(path)) {
        if (part == ".") {
            continue;
        }
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
            continue;
        }
        parts.push_back(std::move(part));
    }
    return parts;
}

std::string join_path(const std::vector<std::string>& components) {
    if (components.empty()) {
        return "/";
    }
    std::string joined;
    for (const std::string& part : components) {
        joined.push_back('/');
        joined.append(part);
    }
    return joined;
}

bool wildcard_match(std::string_view pattern, std::string_view name) {
    std::size_t p = 0;
    std::size_t v = 0;
    std::size_t star = std::string_view::npos;
    std::size_t match = 0;
    while (v < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[v])) {
            ++p;
            ++v;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = v;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            v = ++match;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && (pattern[p] == '*' || pattern[p] == '?')) {
        ++p;
    }
    return p == pattern.size();
}

bool has_wildcard(std::string_view name) {
    return name.find_first_of("*?") != std::string_view::npos;
}

} // namespace dq8::rt::mc
