// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Adapted from PCSX2 v2.7.524 (PCSX2 Dev Team, GPL-3.0-or-later): pcsx2/IPU/yuv2rgb.cpp (yuv2rgb_reference,
// the documented integer method of the IPU) and pcsx2/IPU/IPU_MultiISA.cpp (the SETTH alpha pass of ipu_csc).
#include "runtime/movie/ipu_csc.h"

#include <algorithm>
#include <cstdint>

namespace dq8::rt {

namespace {

// The IPU's coefficients, 1/64 units (yuv2rgb.cpp): 1.1640625, -0.8125, -0.390625, 1.59375, 2.015625.
constexpr std::int32_t y_bias = 16;
constexpr std::int32_t c_bias = 128;
constexpr std::int32_t y_coeff = 0x95;
constexpr std::int32_t gcr_coeff = -0x68;
constexpr std::int32_t gcb_coeff = -0x32;
constexpr std::int32_t rcr_coeff = 0xCC;
constexpr std::int32_t bcb_coeff = 0x102;

constexpr std::uint8_t alpha_opaque = 0x80;
constexpr std::uint8_t alpha_half = 0x40;

std::uint8_t clamp8(std::int32_t v) noexcept {
    return static_cast<std::uint8_t>(std::clamp(v, 0, 255));
}

} // namespace

void ipu_csc_macroblock(const std::uint8_t* y, std::size_t y_stride, const std::uint8_t* cb,
                        const std::uint8_t* cr, std::size_t c_stride, const IpuThresholds& thresholds,
                        std::uint8_t* out_1024) {
    std::uint8_t* p = out_1024;
    for (std::size_t row = 0; row < 16; ++row) {
        const std::uint8_t* yr = y + row * y_stride;
        const std::uint8_t* cbr = cb + (row >> 1) * c_stride;
        const std::uint8_t* crr = cr + (row >> 1) * c_stride;
        for (std::size_t col = 0; col < 16; ++col, p += 4) {
            // Arithmetic right shifts of negative values, as the reference relies on (two's complement).
            const std::int32_t lum = (y_coeff * std::max(0, std::int32_t{yr[col]} - y_bias)) >> 6;
            const std::int32_t crv = std::int32_t{crr[col >> 1]} - c_bias;
            const std::int32_t cbv = std::int32_t{cbr[col >> 1]} - c_bias;
            const std::int32_t rcr = (rcr_coeff * crv) >> 6;
            const std::int32_t gcr = (gcr_coeff * crv) >> 6;
            const std::int32_t gcb = (gcb_coeff * cbv) >> 6;
            const std::int32_t bcb = (bcb_coeff * cbv) >> 6;
            p[0] = clamp8((lum + rcr + 1) >> 1);
            p[1] = clamp8((lum + gcr + gcb + 1) >> 1);
            p[2] = clamp8((lum + bcb + 1) >> 1);
            p[3] = alpha_opaque;
        }
    }

    const auto below = [](const std::uint8_t* px, std::uint16_t th) {
        return px[0] < th && px[1] < th && px[2] < th;
    };
    if (thresholds.th0 > 0) {
        for (p = out_1024; p != out_1024 + 1024; p += 4) {
            if (below(p, thresholds.th0)) {
                p[0] = p[1] = p[2] = p[3] = 0;
            } else if (below(p, thresholds.th1)) {
                p[3] = alpha_half;
            }
        }
    } else if (thresholds.th1 > 0) {
        for (p = out_1024; p != out_1024 + 1024; p += 4) {
            if (below(p, thresholds.th1)) {
                p[3] = alpha_half;
            }
        }
    }
}

} // namespace dq8::rt
