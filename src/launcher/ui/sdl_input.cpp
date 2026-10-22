// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "ui/sdl_input.h"

#include <algorithm>
#include <string>

namespace dq8::launcher::ui {

namespace {

std::string guid_string(SDL_GUID guid) {
    char buf[33] = {};
    SDL_GUIDToString(guid, buf, static_cast<int>(sizeof(buf)));
    return buf;
}

float axis_value(Sint16 raw) {
    return std::clamp(static_cast<float>(raw) / 32767.0f, -1.0f, 1.0f);
}

} // namespace

Gamepads::~Gamepads() {
    for (auto& [id, pad] : pads_) {
        SDL_CloseGamepad(pad.pad);
    }
}

bool Gamepads::on_event(const SDL_Event& event) {
    if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
        const SDL_JoystickID id = event.gdevice.which;
        if (pads_.count(id) != 0) {
            return false;
        }
        SDL_Gamepad* pad = SDL_OpenGamepad(id);
        if (pad == nullptr) {
            return false;
        }
        const char* name = SDL_GetGamepadName(pad);
        pads_[id] = Pad{pad, guid_string(SDL_GetGamepadGUIDForID(id)), name != nullptr ? name : "Gamepad"};
        return true;
    }
    if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
        const auto it = pads_.find(event.gdevice.which);
        if (it == pads_.end()) {
            return false;
        }
        SDL_CloseGamepad(it->second.pad);
        pads_.erase(it);
        return true;
    }
    return false;
}

std::vector<ConnectedPad> Gamepads::connected() const {
    std::vector<ConnectedPad> out;
    for (const auto& [id, pad] : pads_) {
        const bool seen =
            std::any_of(out.begin(), out.end(), [&](const ConnectedPad& p) { return p.guid == pad.guid; });
        if (!seen) {
            out.push_back({pad.guid, pad.name});
        }
    }
    return out;
}

std::string Gamepads::guid_of(SDL_JoystickID id) const {
    const auto it = pads_.find(id);
    return it != pads_.end() ? it->second.guid : std::string();
}

std::vector<SDL_Gamepad*> Gamepads::with_guid(std::string_view guid) const {
    std::vector<SDL_Gamepad*> out;
    for (const auto& [id, pad] : pads_) {
        if (pad.guid == guid) {
            out.push_back(pad.pad);
        }
    }
    return out;
}

std::vector<SDL_Gamepad*> Gamepads::all() const {
    std::vector<SDL_Gamepad*> out;
    for (const auto& [id, pad] : pads_) {
        out.push_back(pad.pad);
    }
    return out;
}

std::optional<InputSignal> to_signal(const SDL_Event& event, const Gamepads& pads) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        if (event.key.repeat) {
            return std::nullopt;
        }
        const char* name = SDL_GetScancodeName(event.key.scancode);
        if (name == nullptr || *name == '\0') {
            return std::nullopt;
        }
        return InputSignal{InputSignal::Kind::key, {}, name, event.type == SDL_EVENT_KEY_DOWN ? 1.0f : 0.0f};
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const char* name =
            SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(event.gbutton.button));
        if (name == nullptr) {
            return std::nullopt;
        }
        return InputSignal{InputSignal::Kind::button, pads.guid_of(event.gbutton.which), name,
                           event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? 1.0f : 0.0f};
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const char* name = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(event.gaxis.axis));
        if (name == nullptr) {
            return std::nullopt;
        }
        return InputSignal{InputSignal::Kind::axis, pads.guid_of(event.gaxis.which), name,
                           axis_value(event.gaxis.value)};
    }
    default:
        return std::nullopt;
    }
}

float source_level(std::string_view source, const DeviceView& device, const Gamepads& pads) {
    if (device.kind == DeviceKind::keyboard) {
        const std::string name(source);
        const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
        if (code == SDL_SCANCODE_UNKNOWN) {
            return 0.0f;
        }
        int count = 0;
        const bool* state = SDL_GetKeyboardState(&count);
        return state != nullptr && static_cast<int>(code) < count && state[code] ? 1.0f : 0.0f;
    }
    const std::vector<SDL_Gamepad*> targets =
        device.kind == DeviceKind::gamepads ? pads.all() : pads.with_guid(device.guid);
    float level = 0.0f;
    if (const auto axis = parse_axis_source(source)) {
        const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(axis->axis.c_str());
        if (a == SDL_GAMEPAD_AXIS_INVALID) {
            return 0.0f;
        }
        for (SDL_Gamepad* pad : targets) {
            const float v = axis_value(SDL_GetGamepadAxis(pad, a)) * static_cast<float>(axis->sign);
            level = std::max(level, v);
        }
        return level;
    }
    const std::string name(source);
    const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name.c_str());
    if (b == SDL_GAMEPAD_BUTTON_INVALID) {
        return 0.0f;
    }
    for (SDL_Gamepad* pad : targets) {
        if (SDL_GetGamepadButton(pad, b)) {
            level = 1.0f;
        }
    }
    return level;
}

} // namespace dq8::launcher::ui
