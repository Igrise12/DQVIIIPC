// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/platform/platform.h"

#include <cstdint>
#include <optional>

struct SDL_Window;

// Extras of the SDL3 platform that are not part of IPlatform: a copy of what the window shows, for tools and
// tests (tools/gs/dq8-gs-replay --window-capture), and the window's state (M5 session C). Main thread only,
// like the platform. Each returns false, null or nothing for an IPlatform not made by create_sdl_platform().
namespace dq8::rt::sdl {

// Asks a platform made by create_sdl_platform() to keep a copy of the window contents at its next present().
// False for any other IPlatform.
bool request_window_capture(IPlatform& platform);
// The copy taken by the last present() after request_window_capture(), as RGBA8; false when there is none.
bool take_window_capture(IPlatform& platform, gs::FrameImage& out);

// The SDL window.
SDL_Window* window_of(IPlatform& platform);
// Whether the window is in full screen now (the window mode, then Alt+Enter).
bool is_fullscreen(IPlatform& platform);
// The pad port (0 or 1) the gamepad with this SDL joystick id drives, if it has one.
std::optional<unsigned> gamepad_port(IPlatform& platform, std::uint32_t joystick_id);

} // namespace dq8::rt::sdl
