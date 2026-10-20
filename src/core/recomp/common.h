// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Small helpers shared by the recomp stages (internal to libdq8core).
namespace dq8::core::recomp::detail {

// "0x00461C80": the recompiler's and the maps' address spelling.
inline std::string hex8(std::uint32_t v) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out = "0x00000000";
    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(9 - i)] = digits[(v >> (4 * i)) & 0xFu];
    }
    return out;
}

// A u32 written as "0x" hex or plain decimal, nothing else (no sign, no spaces, no octal).
inline std::optional<std::uint32_t> parse_u32(std::string_view s) {
    std::uint64_t value = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        if (s.empty() || s.size() > 8) {
            return std::nullopt;
        }
        for (const char c : s) {
            std::uint32_t d = 0;
            if (c >= '0' && c <= '9') {
                d = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                d = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return std::nullopt;
            }
            value = value * 16 + d;
        }
        return static_cast<std::uint32_t>(value);
    }
    if (s.empty() || s.size() > 10) {
        return std::nullopt;
    }
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (value > 0xFFFFFFFFu) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

// UTF-8 spelling of a path, for TOML files and messages.
inline std::string path_utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

} // namespace dq8::core::recomp::detail
