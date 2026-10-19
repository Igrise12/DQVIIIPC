// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/hash.h"

#include <algorithm>
#include <cstring>

namespace dq8::core {

namespace {

constexpr std::uint32_t rotl(std::uint32_t x, unsigned n) noexcept {
    return (x << n) | (x >> (32u - n));
}

constexpr std::array<std::uint32_t, 256> make_crc_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> crc_table = make_crc_table();

} // namespace

Sha1::Sha1() noexcept {
    reset();
}

void Sha1::reset() noexcept {
    state_ = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    buffered_ = 0;
    total_bytes_ = 0;
    buffer_.fill(0);
}

void Sha1::process_block(const std::uint8_t* block) noexcept {
    std::uint32_t w[80];
    for (int t = 0; t < 16; ++t) {
        w[t] = (static_cast<std::uint32_t>(block[t * 4]) << 24) |
               (static_cast<std::uint32_t>(block[t * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[t * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[t * 4 + 3]);
    }
    for (int t = 16; t < 80; ++t) {
        w[t] = rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    }
    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];
    for (int t = 0; t < 80; ++t) {
        std::uint32_t f;
        std::uint32_t k;
        if (t < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (t < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (t < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const std::uint32_t temp = rotl(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = temp;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
}

void Sha1::update(std::span<const std::uint8_t> data) noexcept {
    total_bytes_ += data.size();
    std::size_t pos = 0;
    if (buffered_ > 0) {
        const std::size_t take = std::min(data.size(), buffer_.size() - buffered_);
        std::memcpy(buffer_.data() + buffered_, data.data(), take);
        buffered_ += take;
        pos += take;
        if (buffered_ == buffer_.size()) {
            process_block(buffer_.data());
            buffered_ = 0;
        }
    }
    while (data.size() - pos >= 64) {
        process_block(data.data() + pos);
        pos += 64;
    }
    if (pos < data.size()) {
        std::memcpy(buffer_.data(), data.data() + pos, data.size() - pos);
        buffered_ = data.size() - pos;
    }
}

Sha1::Digest Sha1::finish() noexcept {
    const std::uint64_t bit_length = total_bytes_ * 8;
    const std::uint8_t pad = 0x80;
    update(std::span<const std::uint8_t>(&pad, 1));
    const std::uint8_t zero = 0;
    while (buffered_ != 56) {
        update(std::span<const std::uint8_t>(&zero, 1));
    }
    std::array<std::uint8_t, 8> length_bytes{};
    for (int i = 0; i < 8; ++i) {
        length_bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bit_length >> (56 - 8 * i));
    }
    update(length_bytes);
    Digest digest{};
    for (std::size_t i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
    }
    return digest;
}

Sha1::Digest Sha1::of(std::span<const std::uint8_t> data) noexcept {
    Sha1 h;
    h.update(data);
    return h.finish();
}

void Crc32::update(std::span<const std::uint8_t> data) noexcept {
    std::uint32_t c = crc_;
    for (const std::uint8_t byte : data) {
        c = crc_table[(c ^ byte) & 0xFFu] ^ (c >> 8);
    }
    crc_ = c;
}

std::uint32_t Crc32::of(std::span<const std::uint8_t> data) noexcept {
    Crc32 c;
    c.update(data);
    return c.value();
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0Fu]);
    }
    return out;
}

std::string to_hex(std::uint32_t value) {
    const std::array<std::uint8_t, 4> bytes{
        static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16),
        static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
    return to_hex(bytes);
}

std::string normalize_hex(std::string_view hex) {
    std::string out(hex);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'F') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

} // namespace dq8::core
