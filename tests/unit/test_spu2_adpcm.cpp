// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The SPU2's ADPCM decoder (spu2::decode_adpcm_block) against an independent reference written from the
// format's description, on authored blocks: every filter and every shift, chains that carry the history,
// and the clamps. The decoded PCM of the whole set is pinned to a golden hash.
#include "runtime/spu2/spu2_chip.h"
#include "test_spu2_rig.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

using namespace dq8::test::spu2;
namespace chip = dq8::rt::spu2;

namespace {

// floor(a / 2^n) without shifts of negative numbers.
std::int64_t floor_div_pow2(std::int64_t a, int n) {
    const std::int64_t d = std::int64_t{1} << n;
    std::int64_t q = a / d;
    if (a % d != 0 && a < 0) {
        --q;
    }
    return q;
}

// The reference: each sample is the signed nibble scaled to the top of a 16-bit word and divided by 2^shift
// (rounding down), plus the prediction (pos * s[-1] + neg * s[-2] + 32) / 64 (rounding down), clamped to
// 16 bits. Filters 0-4 have the documented factors; the others predict nothing.
struct Reference {
    std::int64_t old = 0;
    std::int64_t older = 0;

    std::vector<std::int16_t> decode(const std::array<std::uint16_t, 8>& block) {
        static const int pos[5] = {0, 60, 115, 98, 122};
        static const int neg[5] = {0, 0, -52, -55, -60};
        const int shift = block[0] & 0xF;
        const int filter = (block[0] >> 4) & 0xF;
        const int f0 = filter < 5 ? pos[filter] : 0;
        const int f1 = filter < 5 ? neg[filter] : 0;
        std::vector<std::int16_t> out;
        for (int i = 0; i < 28; ++i) {
            const int byte_index = i / 2; // bytes 2..15 of the block
            const int word = block[1 + static_cast<std::size_t>(byte_index / 2)];
            const int byte = (byte_index % 2 == 0) ? (word & 0xFF) : (word >> 8);
            int nibble = (i % 2 == 0) ? (byte & 0xF) : (byte >> 4);
            if (nibble >= 8) {
                nibble -= 16;
            }
            std::int64_t s = floor_div_pow2(std::int64_t{nibble} * 4096, shift);
            s += floor_div_pow2(f0 * old + f1 * older + 32, 6);
            if (s > 32767) {
                s = 32767;
            }
            if (s < -32768) {
                s = -32768;
            }
            older = old;
            old = s;
            out.push_back(static_cast<std::int16_t>(s));
        }
        return out;
    }
};

} // namespace

TEST_CASE("spu2 ADPCM: every filter and shift matches the reference decoder", "[spu2][adpcm]") {
    Lcg lcg(0x5EED);
    dq8::core::Sha1 golden;
    std::size_t blocks = 0;
    for (std::uint32_t filter : {0u, 1u, 2u, 3u, 4u, 5u, 15u}) {
        for (std::uint32_t shift = 0; shift < 16; ++shift) {
            // A chain of four blocks: the history carries across block boundaries.
            Reference ref;
            std::int32_t prev1 = 0;
            std::int32_t prev2 = 0;
            for (int k = 0; k < 4; ++k) {
                const auto nibbles = random_nibbles(lcg);
                const auto block = adpcm_block(shift, filter, 0, nibbles);
                std::array<std::int16_t, 28> pcm{};
                chip::decode_adpcm_block(block, pcm, prev1, prev2);
                const std::vector<std::int16_t> expected = ref.decode(block);
                for (std::size_t i = 0; i < 28; ++i) {
                    INFO("filter " << filter << " shift " << shift << " block " << k << " sample " << i);
                    REQUIRE(pcm[i] == expected[i]);
                }
                CHECK(prev1 == pcm[27]);
                CHECK(prev2 == pcm[26]);
                for (const std::int16_t s : pcm) {
                    const auto u = static_cast<std::uint16_t>(s);
                    const std::array<std::uint8_t, 2> b = {static_cast<std::uint8_t>(u),
                                                           static_cast<std::uint8_t>(u >> 8)};
                    golden.update(b);
                }
                ++blocks;
            }
        }
    }
    CHECK(blocks == 7 * 16 * 4);
    CHECK(dq8::core::to_hex(golden.finish()) == "5add4654ff4a14da271a095030f5b93b2b85d18e");
}

TEST_CASE("spu2 ADPCM: the prediction saturates at both ends", "[spu2][adpcm]") {
    for (const std::uint8_t n : {std::uint8_t{7}, std::uint8_t{8}}) {
        std::array<std::uint8_t, 28> nibbles{};
        nibbles.fill(n);
        const auto block = adpcm_block(0, 4, 0, nibbles);
        std::int32_t prev1 = 0;
        std::int32_t prev2 = 0;
        std::array<std::int16_t, 28> pcm{};
        dq8::rt::spu2::decode_adpcm_block(block, pcm, prev1, prev2);
        Reference ref;
        const std::vector<std::int16_t> expected = ref.decode(block);
        for (std::size_t i = 0; i < 28; ++i) {
            REQUIRE(pcm[i] == expected[i]);
        }
        CHECK(pcm[27] == (n == 7 ? 32767 : -32768));
    }
}
