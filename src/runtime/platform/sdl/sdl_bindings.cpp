// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Input profiles resolved to SDL3 codes (sdl_bindings.h). M5 session C.
#include "runtime/platform/sdl/sdl_bindings.h"

#include "runtime/platform/sdl/sdl_mapping.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <algorithm>
#include <cmath>
#include <optional>

namespace dq8::rt::sdl {

namespace {

std::optional<Source> key_source(const std::string& name) {
    const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
    if (code == SDL_SCANCODE_UNKNOWN) {
        return std::nullopt;
    }
    return Source{Source::Kind::key, static_cast<int>(code), 1};
}

std::optional<Source> gamepad_source(const std::string& name) {
    const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(name.c_str());
    if (button != SDL_GAMEPAD_BUTTON_INVALID) {
        return Source{Source::Kind::button, static_cast<int>(button), 1};
    }
    // "leftx-", "lefty+", "lefttrigger": SDL reads a leading sign only, so take a trailing one off first.
    std::string axis_name = name;
    int sign = 1;
    if (!axis_name.empty() && (axis_name.back() == '+' || axis_name.back() == '-')) {
        sign = axis_name.back() == '-' ? -1 : 1;
        axis_name.pop_back();
    }
    if (axis_name.empty() || axis_name.front() == '+' || axis_name.front() == '-') {
        return std::nullopt;
    }
    const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(axis_name.c_str());
    if (axis == SDL_GAMEPAD_AXIS_INVALID) {
        return std::nullopt;
    }
    return Source{Source::Kind::axis, static_cast<int>(axis), sign};
}

template <typename Resolve>
DeviceMap resolve(const core::input::Bindings& bindings, std::vector<std::string>* unknown, Resolve&& one) {
    DeviceMap map;
    for (std::size_t i = 0; i < core::input::control_count; ++i) {
        for (const std::string& name : bindings.sources[i]) {
            if (const auto source = one(name)) {
                map.controls[i].push_back(*source);
            } else if (unknown != nullptr) {
                unknown->push_back(name);
            }
        }
    }
    return map;
}

} // namespace

DeviceMap keyboard_map(const core::input::Bindings& bindings, std::vector<std::string>* unknown) {
    return resolve(bindings, unknown, key_source);
}

DeviceMap gamepad_map(const core::input::Bindings& bindings, std::vector<std::string>* unknown) {
    return resolve(bindings, unknown, gamepad_source);
}

float axis_amount(std::int16_t axis) noexcept {
    return std::clamp(static_cast<float>(axis) / 32767.0f, -1.0f, 1.0f);
}

std::uint8_t stick_byte_of(float value) noexcept {
    const float v = std::clamp(value, -1.0f, 1.0f);
    return stick_byte(static_cast<std::int16_t>(std::lround(v * 32767.0f)));
}

void apply_deadzone(float& x, float& y, float deadzone) noexcept {
    deadzone = std::clamp(deadzone, 0.0f, 0.99f);
    if (deadzone <= 0.0f) {
        return;
    }
    const float magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= deadzone) {
        x = 0.0f;
        y = 0.0f;
        return;
    }
    const float scale = (magnitude - deadzone) / (1.0f - deadzone) / magnitude;
    x = std::clamp(x * scale, -1.0f, 1.0f);
    y = std::clamp(y * scale, -1.0f, 1.0f);
}

} // namespace dq8::rt::sdl
