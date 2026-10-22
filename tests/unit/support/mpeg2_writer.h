// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// A minimal MPEG-2 video writer for the movie tests: no game data and no binary fixtures, every stream is
// generated when a test runs. It writes valid Main profile 4:2:0 progressive frame pictures in which every
// macroblock is intra-coded with DC coefficients only, so each 8 x 8 block decodes to one flat value: the
// luma of macroblock (x, y) of a picture is mpeg2_test_luma(seed, x, y) and its chroma is the picture's cb
// and cr. P and B pictures are typed as such (the decoder reorders them) but use no motion compensation.
namespace dq8::test {

struct Mpeg2TestPicture {
    char type = 'I'; // 'I', 'P' or 'B', in decode order
    std::uint32_t seed = 0;
    std::uint8_t cb = 128;
    std::uint8_t cr = 128;
    std::uint32_t temporal_reference = 0; // display position inside the GOP
    bool gop_header = false; // a GOP header (and, for the first picture, the sequence headers) first
};

struct Mpeg2TestStream {
    std::uint32_t width = 64;
    std::uint32_t height = 48;
    bool low_delay = false;
    bool sequence_end = true;
    std::vector<Mpeg2TestPicture> pictures;
};

// The luma DC of a macroblock: a value from 16 to 235 that differs between neighbours and between seeds.
std::uint8_t mpeg2_test_luma(std::uint32_t seed, std::uint32_t mbx, std::uint32_t mby);

// The elementary stream: sequence header and extension, then the pictures, then a sequence end code.
std::vector<std::uint8_t> write_mpeg2_stream(const Mpeg2TestStream& stream);

// A GOP in decode order with `b_frames` B pictures between references (I P B B P B B ... for 2): `count`
// pictures, seeds 1, 2, 3... in display order, temporal references set, a GOP header on the first picture.
// Returns the pictures and fills `display_seeds` with the seeds in display order.
std::vector<Mpeg2TestPicture> mpeg2_test_gop(std::uint32_t count, std::uint32_t b_frames,
                                             std::vector<std::uint32_t>& display_seeds);

// Byte offsets of every picture start code (00 00 01 00) in `es`.
std::vector<std::size_t> mpeg2_picture_offsets(std::span<const std::uint8_t> es);

} // namespace dq8::test
