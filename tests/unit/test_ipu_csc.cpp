// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The IPU colour conversion and the RGB32 macroblock layout of the sceMpeg HLE (runtime/movie/ipu_csc.h,
// macroblock_writer.h), and the SETTH thresholds the IPU register stub keeps.
#include "runtime/movie/ipu_csc.h"
#include "runtime/movie/ipu_regs.h"
#include "runtime/movie/macroblock_writer.h"
#include "support/fake_machine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace dq8::rt;

namespace {

struct Rgba {
    std::uint8_t r, g, b, a;
    bool operator==(const Rgba&) const = default;
};

// One flat macroblock through the CSC; returns its first pixel (all 256 must be equal).
Rgba flat(std::uint8_t y, std::uint8_t cb, std::uint8_t cr, IpuThresholds th = {}) {
    std::array<std::uint8_t, 256> yp;
    std::array<std::uint8_t, 64> cbp;
    std::array<std::uint8_t, 64> crp;
    yp.fill(y);
    cbp.fill(cb);
    crp.fill(cr);
    std::array<std::uint8_t, 1024> out{};
    ipu_csc_macroblock(yp.data(), 16, cbp.data(), crp.data(), 8, th, out.data());
    for (std::size_t i = 4; i < out.size(); i += 4) {
        REQUIRE(out[i] == out[0]);
        REQUIRE(out[i + 1] == out[1]);
        REQUIRE(out[i + 2] == out[2]);
        REQUIRE(out[i + 3] == out[3]);
    }
    return {out[0], out[1], out[2], out[3]};
}

} // namespace

TEST_CASE("IPU CSC: the IPU's integer conversion", "[movie]") {
    CHECK(flat(16, 128, 128) == Rgba{0, 0, 0, 0x80});
    CHECK(flat(235, 128, 128) == Rgba{255, 255, 255, 0x80});
    CHECK(flat(128, 128, 128) == Rgba{130, 130, 130, 0x80});
    CHECK(flat(0, 128, 128) == Rgba{0, 0, 0, 0x80}); // luma below the bias clamps to 0
    // A saturated red: R from Cr, G and B clamped at 0 (the shifts of negative terms round down).
    CHECK(flat(81, 90, 240) == Rgba{254, 0, 0, 0x80});
    // A blue: lum 149*25>>6 = 58; bcb = 258*112>>6 = 451; gcb = -50*112>>6 = -88; rcr = gcr's Cr part 0.
    CHECK(flat(41, 240, 128) == Rgba{29, 0, 255, 0x80});
}

TEST_CASE("IPU CSC: SETTH thresholds set the alpha of dark pixels", "[movie]") {
    const IpuThresholds both{0x10, 0x80};
    CHECK(flat(16, 128, 128, both) == Rgba{0, 0, 0, 0});        // below th0: transparent black
    CHECK(flat(100, 128, 128, both) == Rgba{98, 98, 98, 0x40}); // below th1
    CHECK(flat(128, 128, 128, both) == Rgba{130, 130, 130, 0x80});
    const IpuThresholds only_th1{0, 0x80};
    CHECK(flat(16, 128, 128, only_th1) == Rgba{0, 0, 0, 0x40});
    CHECK(flat(100, 128, 128, only_th1) == Rgba{98, 98, 98, 0x40});
}

TEST_CASE("RGB32 macroblocks: column-major 1 KiB tiles with a padded edge", "[movie]") {
    // 40 x 20: 3 columns by 2 rows of macroblocks, the last column and row partly outside the picture.
    const std::uint32_t w = 40;
    const std::uint32_t h = 20;
    std::vector<std::uint8_t> y(w * h);
    std::vector<std::uint8_t> c((w / 2) * (h / 2), 128);
    for (std::uint32_t row = 0; row < h; ++row) {
        for (std::uint32_t col = 0; col < w; ++col) {
            y[row * w + col] = static_cast<std::uint8_t>(16 + (col / 16) * 60 + (row / 16) * 20);
        }
    }
    PictureYuv p;
    p.width = w;
    p.height = h;
    p.y_stride = w;
    p.c_stride = w / 2;
    p.y = y.data();
    p.cb = c.data();
    p.cr = c.data();

    std::vector<std::uint8_t> out;
    REQUIRE(convert_rgb32_macroblocks(p, 1000, {}, out) == 6);
    REQUIRE(out.size() == 6 * macroblock_rgb32_bytes);
    // Tile k is macroblock (k / 2, k % 2): down the first column, then the next.
    const auto red_at = [&](std::uint32_t tile, std::uint32_t px, std::uint32_t py) {
        return out[tile * macroblock_rgb32_bytes + py * 64 + px * 4];
    };
    const auto grey = [](int luma) {
        return static_cast<std::uint8_t>(((0x95 * (luma - 16) >> 6) + 1) >> 1);
    };
    CHECK(red_at(0, 0, 0) == grey(16));
    CHECK(red_at(1, 0, 0) == grey(36));  // (0, 1)
    CHECK(red_at(2, 0, 0) == grey(76));  // (1, 0)
    CHECK(red_at(3, 0, 0) == grey(96));  // (1, 1)
    CHECK(red_at(4, 7, 3) == grey(136)); // (2, 0) inside the picture
    CHECK(red_at(4, 8, 3) == 0);         // (2, 0) past the right edge: black
    CHECK(red_at(5, 0, 3) == grey(156)); // (2, 1) inside
    CHECK(red_at(5, 0, 4) == 0);         // (2, 1) past the bottom edge
    CHECK(out[5 * macroblock_rgb32_bytes + 4 * 64 + 3] == 0x80);

    // The destination's macroblock count limits the output, in the same order.
    std::vector<std::uint8_t> head;
    REQUIRE(convert_rgb32_macroblocks(p, 3, {}, head) == 3);
    CHECK(std::vector<std::uint8_t>(out.begin(), out.begin() + 3 * macroblock_rgb32_bytes) == head);
}

TEST_CASE("RGB32 macroblocks: a black picture is opaque black", "[movie]") {
    BlackPicture black;
    const PictureYuv& p = black.get(32, 32);
    std::vector<std::uint8_t> out;
    REQUIRE(convert_rgb32_macroblocks(p, 4, {}, out) == 4);
    for (std::size_t i = 0; i < out.size(); i += 4) {
        REQUIRE(out[i] == 0);
        REQUIRE(out[i + 3] == 0x80);
    }
    CHECK(black.get(48, 16).width == 48);
}

TEST_CASE("IPU stub: SETTH's thresholds are kept until reset", "[movie]") {
    dq8::test::FakeMachine m;
    IpuRegisters ipu;
    m.attach(ipu);
    CHECK(IpuRegisters::find(m) == &ipu);
    const MmioContext at{};
    const std::uint32_t setth = (0x9u << 28) | (0x0A0u << 16) | 0x105u;
    std::uint8_t bytes[4];
    std::memcpy(bytes, &setth, 4);
    REQUIRE(m.mmio().write(IpuRegisters::regs_base, 4, bytes, at));
    CHECK(ipu.thresholds().th0 == 0x105);
    CHECK(ipu.thresholds().th1 == 0x0A0);
    ipu.reset();
    CHECK(ipu.thresholds().th0 == 0);
    CHECK(ipu.thresholds().th1 == 0);

    dq8::test::FakeMachine empty;
    CHECK(IpuRegisters::find(empty) == nullptr);
}
