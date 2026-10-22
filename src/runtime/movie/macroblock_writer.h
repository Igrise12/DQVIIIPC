// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
#pragma once

#include "runtime/movie/ipu_csc.h"
#include "runtime/movie/movie_decoder.h"

#include <cstdint>
#include <vector>

// The RGB32 picture layout sceMpegGetPicture leaves in its destination (architecture §7.2): 16 x 16 RGBA32
// macroblocks of 1,024 bytes (16 rows of 64 bytes), column-major: every macroblock of the leftmost column
// from top to bottom, then the next column. Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp
// contributors: ps2xRuntime/src/lib/Kernel/Stubs/MPEG.cpp (writeDecodedFrameToGuest, writeBlankMpegFrame).
namespace dq8::rt {

inline constexpr std::uint32_t macroblock_rgb32_bytes = 1024;

// Converts `picture` (4:2:0) through the IPU CSC into `out`: at most `max_macroblocks` macroblocks in the
// order above, over ceil(width / 16) x ceil(height / 16) macroblocks; samples outside the picture are black
// (Y 16, Cb and Cr 128). Returns the number of macroblocks written (out.size() / 1024).
std::uint32_t convert_rgb32_macroblocks(const PictureYuv& picture, std::uint32_t max_macroblocks,
                                        const IpuThresholds& thresholds, std::vector<std::uint8_t>& out);

// Planes of one black picture (Y 16, Cb and Cr 128) and their PictureYuv view, for pictures no decoder
// painted.
class BlackPicture {
  public:
    // A view of a black width x height picture; valid until the next call.
    const PictureYuv& get(std::uint32_t width, std::uint32_t height);

  private:
    std::vector<std::uint8_t> y_;
    std::vector<std::uint8_t> c_;
    PictureYuv view_;
};

} // namespace dq8::rt
