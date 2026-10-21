// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "gs_state_restore.h"

#include <array>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace dq8::tools::gs {

namespace {

// GS general register numbers (A+D addresses), from the GS hardware register map.
enum Reg : std::uint8_t {
    prim = 0x00,
    rgbaq = 0x01,
    st = 0x02,
    uv = 0x03,
    tex0_1 = 0x06,
    clamp_1 = 0x08,
    tex1_1 = 0x14,
    xyoffset_1 = 0x18,
    prmodecont = 0x1A,
    texclut = 0x1C,
    scanmsk = 0x22,
    miptbp1_1 = 0x34,
    miptbp2_1 = 0x36,
    texa = 0x3B,
    fogcol = 0x3D,
    texflush = 0x3F,
    scissor_1 = 0x40,
    alpha_1 = 0x42,
    dimx = 0x44,
    dthe = 0x45,
    colclamp = 0x46,
    test_1 = 0x47,
    pabe = 0x49,
    fba_1 = 0x4A,
    frame_1 = 0x4C,
    zbuf_1 = 0x4E,
    bitbltbuf = 0x50,
    trxpos = 0x51,
    trxreg = 0x52,
    trxdir = 0x53,
};

// PSMCT32 layout: a page is 64x32 pixels (8 KiB, 32 blocks), a block 8x8 pixels (256 bytes, 64 words).
constexpr std::array<std::array<std::uint8_t, 8>, 4> block_in_page = {{
    {0, 1, 4, 5, 16, 17, 20, 21},
    {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29},
    {10, 11, 14, 15, 26, 27, 30, 31},
}};
// Word within a block: four columns of 8x2 pixels, each column 16 words.
constexpr std::array<std::array<std::uint8_t, 8>, 8> word_in_block = {{
    {0, 1, 4, 5, 8, 9, 12, 13},
    {2, 3, 6, 7, 10, 11, 14, 15},
    {16, 17, 20, 21, 24, 25, 28, 29},
    {18, 19, 22, 23, 26, 27, 30, 31},
    {32, 33, 36, 37, 40, 41, 44, 45},
    {34, 35, 38, 39, 42, 43, 46, 47},
    {48, 49, 52, 53, 56, 57, 60, 61},
    {50, 51, 54, 55, 58, 59, 62, 63},
}};

constexpr std::uint32_t image_width = 1024;  // pixels; 16 pages of 64
constexpr std::uint32_t image_height = 1024; // 32 pages of 32 rows: 512 pages = 4 MiB
constexpr std::uint32_t image_bw = image_width / 64;
constexpr std::uint32_t max_nloop = 0x7FFF;

constexpr std::uint64_t flg_packed = 0;
constexpr std::uint64_t flg_image = 2;
constexpr std::uint64_t reg_a_d = 0xE;

class GifWriter {
  public:
    void tag(std::uint32_t nloop, bool eop, std::uint64_t flg, std::uint64_t nreg, std::uint64_t regs) {
        const std::uint64_t lo = (std::uint64_t{nloop} & max_nloop) | (std::uint64_t{eop} << 15) |
                                 (flg << 58) | ((nreg & 0xF) << 60);
        qword(lo, regs);
    }
    void qword(std::uint64_t lo, std::uint64_t hi) {
        const std::size_t at = out_.size();
        out_.resize(at + 16);
        std::memcpy(out_.data() + at, &lo, 8);
        std::memcpy(out_.data() + at + 8, &hi, 8);
    }
    // One PACKED A+D packet for a list of (register, value) writes.
    void a_d(std::initializer_list<std::pair<std::uint8_t, std::uint64_t>> writes) {
        tag(static_cast<std::uint32_t>(writes.size()), true, flg_packed, 1, reg_a_d);
        for (const auto& [reg, value] : writes) {
            qword(value, reg);
        }
    }
    void bytes(const std::uint8_t* data, std::size_t n) { out_.insert(out_.end(), data, data + n); }
    std::vector<std::uint8_t> take() { return std::move(out_); }

  private:
    std::vector<std::uint8_t> out_;
};

} // namespace

std::uint32_t psmct32_word_index(std::uint32_t x, std::uint32_t y, std::uint32_t bw) noexcept {
    const std::uint32_t page = (y / 32) * bw + x / 64;
    const std::uint32_t px = x % 64;
    const std::uint32_t py = y % 32;
    const std::uint32_t block = block_in_page[py / 8][px / 8];
    const std::uint32_t word = word_in_block[py % 8][px % 8];
    return page * 2048 + block * 64 + word;
}

std::vector<std::uint8_t> state_restore_packets(const DumpState& s) {
    GifWriter gif;

    // 1. GS memory as one PSMCT32 rectangle.
    const std::uint64_t upload_bitbltbuf = std::uint64_t{image_bw} << 48; // DBP 0, DBW 16, DPSM PSMCT32
    const std::uint64_t upload_trxreg = std::uint64_t{image_width} | (std::uint64_t{image_height} << 32);
    gif.a_d({{bitbltbuf, upload_bitbltbuf}, {trxpos, 0}, {trxreg, upload_trxreg}, {trxdir, 0}});

    std::vector<std::uint32_t> raster(std::size_t{image_width} * image_height);
    const auto* words = reinterpret_cast<const std::uint8_t*>(s.vram.data());
    for (std::uint32_t y = 0; y < image_height; ++y) {
        for (std::uint32_t x = 0; x < image_width; ++x) {
            std::uint32_t v = 0;
            std::memcpy(&v, words + std::size_t{psmct32_word_index(x, y, image_bw)} * 4, 4);
            raster[std::size_t{y} * image_width + x] = v;
        }
    }
    const auto* pixels = reinterpret_cast<const std::uint8_t*>(raster.data());
    std::size_t qwords_left = raster.size() * 4 / 16;
    std::size_t offset = 0;
    while (qwords_left != 0) {
        const auto n = static_cast<std::uint32_t>(qwords_left < max_nloop ? qwords_left : max_nloop);
        qwords_left -= n;
        gif.tag(n, qwords_left == 0, flg_image, 0, 0);
        gif.bytes(pixels + offset, std::size_t{n} * 16);
        offset += std::size_t{n} * 16;
    }

    // 2. Registers. TEX0's CLD (bits 61..63) is cleared so no CLUT load is triggered.
    constexpr std::uint64_t tex0_cld_mask = ~(std::uint64_t{7} << 61);
    gif.a_d({{texflush, 0},
             {prmodecont, s.prmodecont},
             {prim, s.prim},
             {texclut, s.texclut},
             {scanmsk, s.scanmsk},
             {texa, s.texa},
             {fogcol, s.fogcol},
             {dimx, s.dimx},
             {dthe, s.dthe},
             {colclamp, s.colclamp},
             {pabe, s.pabe},
             {bitbltbuf, s.bitbltbuf},
             {trxpos, s.trxpos},
             {trxreg, s.trxreg},
             {rgbaq, s.rgbaq},
             {st, s.st},
             {uv, s.uv}});
    for (std::uint8_t i = 0; i < 2; ++i) {
        const DumpState::Context& c = s.ctx[i];
        gif.a_d({{static_cast<std::uint8_t>(xyoffset_1 + i), c.xyoffset},
                 {static_cast<std::uint8_t>(tex0_1 + i), c.tex0 & tex0_cld_mask},
                 {static_cast<std::uint8_t>(tex1_1 + i), c.tex1},
                 {static_cast<std::uint8_t>(clamp_1 + i), c.clamp},
                 {static_cast<std::uint8_t>(miptbp1_1 + i), c.miptbp1},
                 {static_cast<std::uint8_t>(miptbp2_1 + i), c.miptbp2},
                 {static_cast<std::uint8_t>(scissor_1 + i), c.scissor},
                 {static_cast<std::uint8_t>(alpha_1 + i), c.alpha},
                 {static_cast<std::uint8_t>(test_1 + i), c.test},
                 {static_cast<std::uint8_t>(fba_1 + i), c.fba},
                 {static_cast<std::uint8_t>(frame_1 + i), c.frame},
                 {static_cast<std::uint8_t>(zbuf_1 + i), c.zbuf}});
    }
    return gif.take();
}

} // namespace dq8::tools::gs
