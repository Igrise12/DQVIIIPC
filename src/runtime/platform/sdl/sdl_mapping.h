// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/io/pad_state.h"

#include <cstdint>

// Pure helpers of the SDL3 platform, so the tests can check them without a window: the stick conversion, the
// pressure bytes, and where the picture goes in the window. The bindings themselves come from input profiles
// (sdl_bindings.h, dq8/core/input_profile.h; M5 session C); the built-in profile is M3's fixed table:
//
// | Keys (pad port 0) | Gamepad (by position) | Pad |
// |---|---|---|
// | arrow keys | D-pad | D-pad |
// | K, L, J, I | south, east, west, north | Cross, Circle, Square, Triangle |
// | Return, Backspace | start, back | Start, Select |
// | Q, E | shoulders | L1, R1 |
// | 1, 3 | triggers past a quarter | L2, R2 |
// | 2, 4 | stick clicks | L3, R3 |
// | W, A, S, D | left stick | left stick (keys: full deflection) |
// | T, F, G, H | right stick | right stick (keys: full deflection) |
//
// Pressure-sensitive buttons report 255 when pressed and 0 otherwise.
namespace dq8::rt::sdl {

// An SDL stick axis (-32768..32767) as a DualShock 2 stick byte (0..255, 0x80 = centre).
std::uint8_t stick_byte(std::int16_t axis) noexcept;
// Fills `pressure` from the pressed buttons (255 or 0, in the DualShock 2 report order).
void fill_pressure(PadState& state) noexcept;

// A rectangle in the window's output pixels.
struct FitRect {
    float x = 0, y = 0, w = 0, h = 0;
};
// The largest rectangle of display aspect `aspect` (width / height) centred in an output of `out_w` x `out_h`
// pixels. A non-positive aspect keeps the output's own (stretch).
FitRect aspect_fit(int out_w, int out_h, double aspect) noexcept;
// Where a `frame_w` x `frame_h` frame is drawn. Without integer scaling, aspect_fit(). With it, the frame's
// height is scaled by the largest whole number for which the picture still fits at `aspect` (its width
// follows the aspect), or, with aspect 0, each axis by its own largest whole number; the rectangle is centred
// on whole pixels. Falls back to aspect_fit() when the window is smaller than the frame.
FitRect present_rect(int out_w, int out_h, std::uint32_t frame_w, std::uint32_t frame_h, double aspect,
                     bool integer_scaling) noexcept;

} // namespace dq8::rt::sdl
