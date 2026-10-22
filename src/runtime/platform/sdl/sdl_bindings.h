// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/input_profile.h"
#include "runtime/io/pad_state.h"

#include <SDL3/SDL_gamepad.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Input profiles (dq8/core/input_profile.h) resolved to SDL3 codes, and how one device's state becomes the
// virtual DualShock 2's (M5 session C, architecture §8.1). Pure functions apart from the name lookups, so the
// tests check them without a window.
//
// A source gives an amount from 0 to 1: a key or button 0 or 1, a half axis its deflection to that side. A
// button control is pressed when one of its sources passes the source's threshold (a half of the way, a
// quarter for the triggers as in M3). A stick axis is the larger amount of its positive direction's sources
// minus the larger of its negative direction's. The gamepad's sticks pass a radial dead zone first (rescaled,
// so that the stick still reaches full deflection); the triggers do not.
namespace dq8::rt::sdl {

struct Source {
    enum class Kind : std::uint8_t { key, button, axis };
    Kind kind = Kind::key;
    int code = 0; // SDL_Scancode, SDL_GamepadButton or SDL_GamepadAxis
    int sign = 1; // axis: +1 the positive half, -1 the negative half
    friend bool operator==(const Source&, const Source&) = default;
};

struct DeviceMap {
    std::array<std::vector<Source>, core::input::control_count> controls;
    const std::vector<Source>& operator[](core::input::Control c) const {
        return controls[static_cast<std::size_t>(c)];
    }
};

// Resolves names with SDL_GetScancodeFromName, or SDL_GetGamepadButtonFromString and
// SDL_GetGamepadAxisFromString ("leftx-", "lefttrigger"). Names SDL does not know are left out and appended
// to `unknown` when it is given.
DeviceMap keyboard_map(const core::input::Bindings& bindings, std::vector<std::string>* unknown = nullptr);
DeviceMap gamepad_map(const core::input::Bindings& bindings, std::vector<std::string>* unknown = nullptr);

// An SDL axis (-32768..32767) as -1..1.
float axis_amount(std::int16_t axis) noexcept;
// -1..1 as a DualShock 2 stick byte: 0x00, 0x80 at the centre, 0xFF; full deflection gives what M3's
// stick_byte() gave for the end positions.
std::uint8_t stick_byte_of(float value) noexcept;
// The radial dead zone (0..1 of full deflection) on one stick, rescaled to start from 0 at its edge.
void apply_deadzone(float& x, float& y, float deadzone) noexcept;

// Applies `map` to `state`: buttons are OR-ed in; a stick whose deflection is not zero replaces what the
// state had, a centred one leaves it alone (so a gamepad on port 0 overrides the keyboard's stick keys only
// while it is deflected).
template <typename Amount> void apply_map(PadState& state, const DeviceMap& map, Amount&& amount) {
    using core::input::Control;
    static constexpr std::array<std::pair<Control, std::uint16_t>, 16> buttons = {{
        {Control::up, pad_up},
        {Control::down, pad_down},
        {Control::left, pad_left},
        {Control::right, pad_right},
        {Control::triangle, pad_triangle},
        {Control::circle, pad_circle},
        {Control::cross, pad_cross},
        {Control::square, pad_square},
        {Control::l1, pad_l1},
        {Control::r1, pad_r1},
        {Control::l2, pad_l2},
        {Control::r2, pad_r2},
        {Control::l3, pad_l3},
        {Control::r3, pad_r3},
        {Control::start, pad_start},
        {Control::select, pad_select},
    }};
    const auto threshold = [](const Source& s) {
        const bool trigger = s.kind == Source::Kind::axis && (s.code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                                                              s.code == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        return trigger ? 0.25f : 0.5f;
    };
    for (const auto& [control, bit] : buttons) {
        for (const Source& s : map[control]) {
            if (amount(s) > threshold(s)) {
                state.buttons = static_cast<std::uint16_t>(state.buttons | bit);
                break;
            }
        }
    }
    const auto most = [&](Control c) {
        float m = 0.0f;
        for (const Source& s : map[c]) {
            const float a = amount(s);
            m = a > m ? a : m;
        }
        return m;
    };
    const auto stick = [&](Control negative, Control positive, std::uint8_t& value) {
        const float v = most(positive) - most(negative);
        if (v != 0.0f) {
            value = stick_byte_of(v);
        }
    };
    stick(Control::left_stick_left, Control::left_stick_right, state.lx);
    stick(Control::left_stick_up, Control::left_stick_down, state.ly);
    stick(Control::right_stick_left, Control::right_stick_right, state.rx);
    stick(Control::right_stick_up, Control::right_stick_down, state.ry);
}

// A gamepad's live state, read once per pad() call: the four stick axes after the dead zone, the triggers,
// the buttons. amount() answers apply_map().
struct GamepadReading {
    std::array<float, SDL_GAMEPAD_AXIS_COUNT> axes{};     // by SDL_GamepadAxis, -1..1 (triggers 0..1)
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{}; // by SDL_GamepadButton
    float amount(const Source& s) const noexcept {
        switch (s.kind) {
        case Source::Kind::button:
            return s.code >= 0 && static_cast<std::size_t>(s.code) < buttons.size() &&
                           buttons[static_cast<std::size_t>(s.code)]
                       ? 1.0f
                       : 0.0f;
        case Source::Kind::axis: {
            if (s.code < 0 || static_cast<std::size_t>(s.code) >= axes.size()) {
                return 0.0f;
            }
            const float v = axes[static_cast<std::size_t>(s.code)] * static_cast<float>(s.sign);
            return v > 0.0f ? v : 0.0f;
        }
        case Source::Kind::key:
            break;
        }
        return 0.0f;
    }
};

} // namespace dq8::rt::sdl
