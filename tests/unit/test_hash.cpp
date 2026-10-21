// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/hash.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using dq8::core::Crc32;
using dq8::core::Sha1;
using dq8::core::to_hex;

namespace {

std::vector<std::uint8_t> bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

} // namespace

TEST_CASE("SHA-1 test vectors (RFC 3174 and FIPS 180-1)", "[core][hash]") {
    CHECK(to_hex(Sha1::of(bytes(""))) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(to_hex(Sha1::of(bytes("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(to_hex(Sha1::of(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    // One million 'a': the streaming path across many blocks.
    Sha1 h;
    const std::vector<std::uint8_t> chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) {
        h.update(chunk);
    }
    CHECK(to_hex(h.finish()) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST_CASE("SHA-1 streaming equals one-shot for odd chunk sizes", "[core][hash]") {
    std::vector<std::uint8_t> data(1000);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }
    const std::string whole = to_hex(Sha1::of(data));
    for (const std::size_t step : {1u, 3u, 63u, 64u, 65u, 200u}) {
        Sha1 h;
        for (std::size_t off = 0; off < data.size(); off += step) {
            const std::size_t n = std::min(step, data.size() - off);
            h.update(std::span<const std::uint8_t>(data.data() + off, n));
        }
        CHECK(to_hex(h.finish()) == whole);
    }
}

TEST_CASE("CRC-32 check value and streaming", "[core][hash]") {
    CHECK(Crc32::of(bytes("123456789")) == 0xCBF43926u);
    CHECK(to_hex(Crc32::of(bytes("123456789"))) == "cbf43926");
    CHECK(Crc32::of(bytes("")) == 0u);
    Crc32 c;
    c.update(bytes("1234"));
    c.update(bytes("56789"));
    CHECK(c.value() == 0xCBF43926u);
}

TEST_CASE("hex helpers", "[core][hash]") {
    CHECK(to_hex(0xA66A08B0u) == "a66a08b0");
    CHECK(to_hex(0u) == "00000000");
    CHECK(dq8::core::normalize_hex("A66A08b0") == "a66a08b0");
}
