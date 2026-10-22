// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
#pragma once

#include <cstddef>
#include <cstdint>

// The IPU's colour space conversion (CSC), as the sceMpeg HLE applies it to every decoded macroblock so the
// game's RGB32 decode slots hold what the IPU would have written (architecture §7.2, ADR-0015). Adapted from
// PCSX2's reference conversion (pcsx2/IPU/yuv2rgb.cpp, yuv2rgb_reference) and its alpha thresholds
// (pcsx2/IPU/IPU_MultiISA.cpp, ipu_csc), PCSX2 Dev Team, GPL-3.0-or-later.
namespace dq8::rt {

// SETTH's two thresholds (9 bits each): pixels whose R, G and B are all below th0 become fully transparent
// black; otherwise those below th1 get alpha 0x40. Both 0 (the reset value): alpha 0x80 everywhere.
struct IpuThresholds {
    std::uint16_t th0 = 0;
    std::uint16_t th1 = 0;
};

// One 16 x 16 macroblock from 4:2:0 samples (16 x 16 luma, 8 x 8 Cb and Cr) to RGBA32: 16 rows of 64 bytes,
// each pixel R, G, B, A. The planes are read with the given strides.
void ipu_csc_macroblock(const std::uint8_t* y, std::size_t y_stride, const std::uint8_t* cb,
                        const std::uint8_t* cr, std::size_t c_stride, const IpuThresholds& thresholds,
                        std::uint8_t* out_1024);

} // namespace dq8::rt
