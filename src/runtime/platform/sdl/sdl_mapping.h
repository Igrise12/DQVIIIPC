// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/io/pad_state.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <cstdint>

// The fixed M3 input mapping of the SDL3 platform and the presentation's aspect fit, as pure functions so
// the tests can check them without a window. Settings-driven bindings replace the table (architecture §8.1).
//
// Keyboard (pad port 0), close to PCSX2's default keyboard layout:
//
// | Keys | Pad |
// |---|---|
// | arrow keys | D-pad |
// | K, L, J, I | Cross, Circle, Square, Triangle |
// | Return, Backspace | Start, Select |
// | Q, E | L1, R1 |
// | 1, 3 | L2, R2 |
// | 2, 4 | L3, R3 |
// | W, A, S, D | left stick up, left, down, right (full deflection) |
// | T, F, G, H | right stick up, left, down, right (full deflection) |
//
// Gamepads (SDL's standard layout; the first one also drives port 0, the second one port 1): the face buttons
// by position (south = Cross, east = Circle, west = Square, north = Triangle), shoulders L1/R1, triggers
// L2/R2 when pressed past a quarter, stick clicks L3/R3, back = Select, start = Start, D-pad, both sticks.
// Pressure-sensitive buttons report 255 when pressed and 0 otherwise.
namespace dq8::rt::sdl {

// The PadButton bit for a key, or 0.
std::uint16_t button_for_scancode(SDL_Scancode code) noexcept;
// The PadButton bit for a gamepad button, or 0.
std::uint16_t button_for_gamepad_button(SDL_GamepadButton button) noexcept;
// An SDL stick axis (-32768..32767) as a DualShock 2 stick byte (0..255, 0x80 = centre).
std::uint8_t stick_byte(std::int16_t axis) noexcept;
// Whether an SDL trigger axis (0..32767) counts as pressed.
bool trigger_pressed(std::int16_t axis) noexcept;
// Fills `pressure` from the pressed buttons (255 or 0, in the DualShock 2 report order).
void fill_pressure(PadState& state) noexcept;

// Applies the keyboard table above to `state`; `pressed(code)` tells whether a key is down.
template <typename Pressed> void apply_keyboard(PadState& state, Pressed&& pressed) {
    for (int code = 0; code < SDL_SCANCODE_COUNT; ++code) {
        if (pressed(static_cast<SDL_Scancode>(code))) {
            state.buttons = static_cast<std::uint16_t>(state.buttons |
                                                       button_for_scancode(static_cast<SDL_Scancode>(code)));
        }
    }
    const auto axis = [&](SDL_Scancode low, SDL_Scancode high, std::uint8_t& value) {
        const bool lo = pressed(low);
        const bool hi = pressed(high);
        if (lo != hi) {
            value = lo ? 0x00 : 0xFF;
        }
    };
    axis(SDL_SCANCODE_A, SDL_SCANCODE_D, state.lx);
    axis(SDL_SCANCODE_W, SDL_SCANCODE_S, state.ly);
    axis(SDL_SCANCODE_F, SDL_SCANCODE_H, state.rx);
    axis(SDL_SCANCODE_T, SDL_SCANCODE_G, state.ry);
}

// The largest rectangle of display aspect `aspect` (width / height) centred in an output of `out_w` x `out_h`
// pixels. A non-positive aspect keeps the output's own.
struct FitRect {
    float x = 0, y = 0, w = 0, h = 0;
};
FitRect aspect_fit(int out_w, int out_h, double aspect) noexcept;

} // namespace dq8::rt::sdl
