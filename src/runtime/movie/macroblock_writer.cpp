// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// The column-strip layout is adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xRuntime/src/lib/Kernel/Stubs/MPEG.cpp (writeDecodedFrameToGuest, writeBlankMpegFrame).
#include "runtime/movie/macroblock_writer.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace dq8::rt {

namespace {

constexpr std::uint8_t black_y = 16;
constexpr std::uint8_t black_c = 128;

} // namespace

std::uint32_t convert_rgb32_macroblocks(const PictureYuv& picture, std::uint32_t max_macroblocks,
                                        const IpuThresholds& thresholds, std::vector<std::uint8_t>& out) {
    const std::uint32_t columns = (picture.width + 15) / 16;
    const std::uint32_t rows = (picture.height + 15) / 16;
    const std::uint32_t count = std::min(max_macroblocks, columns * rows);
    out.resize(std::size_t{count} * macroblock_rgb32_bytes);

    // A macroblock that runs past the picture's edge is converted from a padded copy.
    std::array<std::uint8_t, 16 * 16> y{};
    std::array<std::uint8_t, 8 * 8> cb{};
    std::array<std::uint8_t, 8 * 8> cr{};
    const std::uint32_t c_width = (picture.width + 1) / 2;
    const std::uint32_t c_height = (picture.height + 1) / 2;

    std::uint32_t written = 0;
    for (std::uint32_t mbx = 0; mbx < columns && written < count; ++mbx) {
        for (std::uint32_t mby = 0; mby < rows && written < count; ++mby, ++written) {
            std::uint8_t* dst = out.data() + std::size_t{written} * macroblock_rgb32_bytes;
            const std::uint32_t x0 = mbx * 16;
            const std::uint32_t y0 = mby * 16;
            if (x0 + 16 <= picture.width && y0 + 16 <= picture.height) {
                ipu_csc_macroblock(picture.y + std::size_t{y0} * picture.y_stride + x0, picture.y_stride,
                                   picture.cb + std::size_t{y0 / 2} * picture.c_stride + x0 / 2,
                                   picture.cr + std::size_t{y0 / 2} * picture.c_stride + x0 / 2,
                                   picture.c_stride, thresholds, dst);
                continue;
            }
            for (std::uint32_t r = 0; r < 16; ++r) {
                for (std::uint32_t c = 0; c < 16; ++c) {
                    const bool inside = x0 + c < picture.width && y0 + r < picture.height;
                    y[r * 16 + c] =
                        inside ? picture.y[std::size_t{y0 + r} * picture.y_stride + x0 + c] : black_y;
                }
            }
            for (std::uint32_t r = 0; r < 8; ++r) {
                for (std::uint32_t c = 0; c < 8; ++c) {
                    const std::uint32_t cx = x0 / 2 + c;
                    const std::uint32_t cy = y0 / 2 + r;
                    const bool inside = cx < c_width && cy < c_height;
                    const std::size_t at = std::size_t{cy} * picture.c_stride + cx;
                    cb[r * 8 + c] = inside ? picture.cb[at] : black_c;
                    cr[r * 8 + c] = inside ? picture.cr[at] : black_c;
                }
            }
            ipu_csc_macroblock(y.data(), 16, cb.data(), cr.data(), 8, thresholds, dst);
        }
    }
    return written;
}

const PictureYuv& BlackPicture::get(std::uint32_t width, std::uint32_t height) {
    const std::uint32_t c_width = (width + 1) / 2;
    const std::uint32_t c_height = (height + 1) / 2;
    if (view_.width != width || view_.height != height || y_.empty()) {
        y_.assign(std::size_t{width} * height, black_y);
        c_.assign(std::size_t{c_width} * c_height, black_c);
        view_ = PictureYuv{};
        view_.width = width;
        view_.height = height;
        view_.y_stride = width;
        view_.c_stride = c_width;
    }
    view_.pts90k = -1;
    view_.y = y_.data();
    view_.cb = c_.data();
    view_.cr = c_.data();
    return view_;
}

} // namespace dq8::rt
