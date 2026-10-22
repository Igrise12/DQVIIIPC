// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/kernel/iop_printf.h"

#include <algorithm>
#include <cstddef>

namespace dq8::rt {

namespace {

std::string digits(std::uint64_t v, unsigned base, bool upper) {
    const char* set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string out;
    do {
        out.push_back(set[v % base]);
        v /= base;
    } while (v != 0);
    std::reverse(out.begin(), out.end());
    return out;
}

} // namespace

std::string iop_format(std::string_view f, const IopPrintfSource& src, unsigned first_slot) {
    std::string out;
    unsigned next = first_slot;
    auto take32 = [&]() { return src.word(next++); };
    auto take64 = [&]() {
        next += next & 1u; // 64-bit arguments start on an even word
        const std::uint64_t lo = src.word(next++);
        const std::uint64_t hi = src.word(next++);
        return lo | (hi << 32);
    };

    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') {
            out.push_back(f[i]);
            continue;
        }
        const std::size_t start = i;
        ++i;
        bool left = false;
        bool plus = false;
        bool space = false;
        bool alt = false;
        bool zero = false;
        for (; i < f.size(); ++i) {
            if (f[i] == '-') {
                left = true;
            } else if (f[i] == '+') {
                plus = true;
            } else if (f[i] == ' ') {
                space = true;
            } else if (f[i] == '#') {
                alt = true;
            } else if (f[i] == '0') {
                zero = true;
            } else {
                break;
            }
        }
        int width = 0;
        if (i < f.size() && f[i] == '*') {
            width = static_cast<int>(take32());
            if (width < 0) {
                left = true;
                width = -width;
            }
            ++i;
        } else {
            while (i < f.size() && f[i] >= '0' && f[i] <= '9') {
                width = std::min(width * 10 + (f[i] - '0'), 4096);
                ++i;
            }
        }
        int precision = -1;
        if (i < f.size() && f[i] == '.') {
            ++i;
            precision = 0;
            if (i < f.size() && f[i] == '*') {
                precision = static_cast<int>(take32());
                ++i;
            } else {
                while (i < f.size() && f[i] >= '0' && f[i] <= '9') {
                    precision = std::min(precision * 10 + (f[i] - '0'), 4096);
                    ++i;
                }
            }
        }
        int longs = 0;
        bool half = false;
        bool byte = false;
        while (i < f.size() && (f[i] == 'l' || f[i] == 'h' || f[i] == 'j' || f[i] == 'z' || f[i] == 't')) {
            if (f[i] == 'l') {
                ++longs;
            } else if (f[i] == 'h') {
                byte = half;
                half = true;
            }
            ++i;
        }
        if (i >= f.size()) {
            out.append(f.substr(start));
            break;
        }
        const char conv = f[i];
        std::string body;
        std::string sign;
        std::string prefix;
        bool numeric = false;
        switch (conv) {
        case '%':
            out.push_back('%');
            continue;
        case 'c':
            body.push_back(static_cast<char>(take32()));
            break;
        case 's': {
            const std::uint32_t p = take32();
            body = p == 0 ? std::string("(null)") : src.string_at(p);
            if (precision >= 0 && body.size() > static_cast<std::size_t>(precision)) {
                body.resize(static_cast<std::size_t>(precision));
            }
            break;
        }
        case 'd':
        case 'i': {
            std::int64_t v = longs >= 2 ? static_cast<std::int64_t>(take64())
                                        : static_cast<std::int64_t>(static_cast<std::int32_t>(take32()));
            if (byte) {
                v = static_cast<std::int8_t>(v);
            } else if (half) {
                v = static_cast<std::int16_t>(v);
            }
            const std::uint64_t mag =
                v < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
            sign = v < 0 ? "-" : plus ? "+" : space ? " " : "";
            body = precision == 0 && mag == 0 ? std::string() : digits(mag, 10, false);
            numeric = true;
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p': {
            std::uint64_t v = longs >= 2 && conv != 'p' ? take64() : take32();
            if (byte) {
                v &= 0xFFu;
            } else if (half) {
                v &= 0xFFFFu;
            }
            const unsigned base = conv == 'u' ? 10 : conv == 'o' ? 8 : 16;
            body = precision == 0 && v == 0 ? std::string() : digits(v, base, conv == 'X');
            if ((alt && v != 0 && base == 16) || conv == 'p') {
                prefix = conv == 'X' ? "0X" : "0x";
            } else if (alt && base == 8 && (body.empty() || body[0] != '0')) {
                prefix = "0";
            }
            numeric = true;
            break;
        }
        default:
            out.append(f.substr(start, i - start + 1));
            continue;
        }
        if (numeric && precision >= 0 && body.size() < static_cast<std::size_t>(precision)) {
            body.insert(0, static_cast<std::size_t>(precision) - body.size(), '0');
        }
        const std::size_t used = sign.size() + prefix.size() + body.size();
        const std::size_t pad =
            static_cast<std::size_t>(width) > used ? static_cast<std::size_t>(width) - used : 0;
        if (left) {
            out += sign + prefix + body + std::string(pad, ' ');
        } else if (zero && numeric && precision < 0) {
            out += sign + prefix + std::string(pad, '0') + body;
        } else {
            out += std::string(pad, ' ') + sign + prefix + body;
        }
    }
    return out;
}

} // namespace dq8::rt
