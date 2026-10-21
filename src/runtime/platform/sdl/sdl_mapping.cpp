// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/platform/sdl/sdl_mapping.h"

#include <array>
#include <cstddef>

namespace dq8::rt::sdl {

std::uint16_t button_for_scancode(SDL_Scancode code) noexcept {
    switch (code) {
    case SDL_SCANCODE_UP:
        return pad_up;
    case SDL_SCANCODE_DOWN:
        return pad_down;
    case SDL_SCANCODE_LEFT:
        return pad_left;
    case SDL_SCANCODE_RIGHT:
        return pad_right;
    case SDL_SCANCODE_K:
        return pad_cross;
    case SDL_SCANCODE_L:
        return pad_circle;
    case SDL_SCANCODE_J:
        return pad_square;
    case SDL_SCANCODE_I:
        return pad_triangle;
    case SDL_SCANCODE_RETURN:
        return pad_start;
    case SDL_SCANCODE_BACKSPACE:
        return pad_select;
    case SDL_SCANCODE_Q:
        return pad_l1;
    case SDL_SCANCODE_E:
        return pad_r1;
    case SDL_SCANCODE_1:
        return pad_l2;
    case SDL_SCANCODE_3:
        return pad_r2;
    case SDL_SCANCODE_2:
        return pad_l3;
    case SDL_SCANCODE_4:
        return pad_r3;
    default:
        return 0;
    }
}

std::uint16_t button_for_gamepad_button(SDL_GamepadButton button) noexcept {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return pad_cross;
    case SDL_GAMEPAD_BUTTON_EAST:
        return pad_circle;
    case SDL_GAMEPAD_BUTTON_WEST:
        return pad_square;
    case SDL_GAMEPAD_BUTTON_NORTH:
        return pad_triangle;
    case SDL_GAMEPAD_BUTTON_BACK:
        return pad_select;
    case SDL_GAMEPAD_BUTTON_START:
        return pad_start;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return pad_l3;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return pad_r3;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return pad_l1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return pad_r1;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return pad_up;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return pad_down;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return pad_left;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return pad_right;
    default:
        return 0;
    }
}

std::uint8_t stick_byte(std::int16_t axis) noexcept {
    return static_cast<std::uint8_t>((static_cast<int>(axis) + 32768) >> 8);
}

bool trigger_pressed(std::int16_t axis) noexcept {
    return axis > 32767 / 4;
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

} // namespace dq8::rt::sdl
