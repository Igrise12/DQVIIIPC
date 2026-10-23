// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/platform/sdl/sdl_mapping.h"

#include <array>
#include <cmath>
#include <cstddef>

namespace dq8::rt::sdl {

std::uint8_t stick_byte(std::int16_t axis) noexcept {
    return static_cast<std::uint8_t>((static_cast<int>(axis) + 32768) >> 8);
}

void fill_pressure(PadState& state) noexcept {
    // The DualShock 2 report order of the pressure bytes.
    static constexpr std::array<std::uint16_t, 12> order = {pad_right,    pad_left,   pad_up,    pad_down,
                                                            pad_triangle, pad_circle, pad_cross, pad_square,
                                                            pad_l1,       pad_r1,     pad_l2,    pad_r2};
    for (std::size_t i = 0; i < order.size(); ++i) {
        state.pressure[i] = (state.buttons & order[i]) != 0 ? 0xFF : 0x00;
    }
}

FitRect aspect_fit(int out_w, int out_h, double aspect) noexcept {
    FitRect r;
    if (out_w <= 0 || out_h <= 0) {
        return r;
    }
    const double w = out_w;
    const double h = out_h;
    if (!(aspect > 0.0)) {
        aspect = w / h;
    }
    double fit_w = w;
    double fit_h = w / aspect;
    if (fit_h > h) {
        fit_h = h;
        fit_w = h * aspect;
    }
    r.w = static_cast<float>(fit_w);
    r.h = static_cast<float>(fit_h);
    r.x = static_cast<float>((w - fit_w) / 2.0);
    r.y = static_cast<float>((h - fit_h) / 2.0);
    return r;
}

FitRect present_rect(int out_w, int out_h, std::uint32_t frame_w, std::uint32_t frame_h, double aspect,
                     bool integer_scaling) noexcept {
    if (!integer_scaling || frame_w == 0 || frame_h == 0 || out_w <= 0 || out_h <= 0) {
        return aspect_fit(out_w, out_h, aspect);
    }
    double w = 0;
    double h = 0;
    if (aspect > 0.0) {
        for (std::uint32_t k = static_cast<std::uint32_t>(out_h) / frame_h; k >= 1; --k) {
            h = static_cast<double>(k) * frame_h;
            w = std::round(h * aspect);
            if (w <= out_w) {
                break;
            }
            h = 0;
        }
    } else {
        const std::uint32_t kx = static_cast<std::uint32_t>(out_w) / frame_w;
        const std::uint32_t ky = static_cast<std::uint32_t>(out_h) / frame_h;
        if (kx >= 1 && ky >= 1) {
            w = static_cast<double>(kx) * frame_w;
            h = static_cast<double>(ky) * frame_h;
        }
    }
    if (h <= 0) {
        return aspect_fit(out_w, out_h, aspect);
    }
    FitRect r;
    r.w = static_cast<float>(w);
    r.h = static_cast<float>(h);
    r.x = static_cast<float>(std::floor((out_w - w) / 2.0));
    r.y = static_cast<float>(std::floor((out_h - h) / 2.0));
    return r;
}

} // namespace dq8::rt::sdl
