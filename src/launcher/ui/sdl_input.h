// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "model/controllers.h"

#include <SDL3/SDL.h>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The Controllers page's link to SDL: the open gamepads by GUID, SDL events as InputSignal values (SDL's own
// names, as session C's profile files use them), and the live level of a source for the test view.
namespace dq8::launcher::ui {

class Gamepads {
  public:
    Gamepads() = default;
    ~Gamepads();
    Gamepads(const Gamepads&) = delete;
    Gamepads& operator=(const Gamepads&) = delete;

    // Opens and closes gamepads as SDL reports them; true when the list changed.
    bool on_event(const SDL_Event& event);
    std::vector<ConnectedPad> connected() const;
    std::string guid_of(SDL_JoystickID id) const;
    // Every open pad with this GUID (two identical pads share one).
    std::vector<SDL_Gamepad*> with_guid(std::string_view guid) const;
    std::vector<SDL_Gamepad*> all() const;

  private:
    struct Pad {
        SDL_Gamepad* pad = nullptr;
        std::string guid;
        std::string name;
    };
    std::map<SDL_JoystickID, Pad> pads_;
};

// A key, gamepad button or axis event as a signal; nullopt for other events.
std::optional<InputSignal> to_signal(const SDL_Event& event, const Gamepads& pads);

// The level (0 to 1) of one source on a device: the keyboard's state for a key name, the pad's button or half
// axis otherwise (all gamepads: the highest over every pad).
float source_level(std::string_view source, const DeviceView& device, const Gamepads& pads);

} // namespace dq8::launcher::ui
