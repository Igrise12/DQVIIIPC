// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/io/pad_state.h"

#include <memory>
#include <string>

// The window, input and presentation of dq8-game (ADR-0005). Main thread only. The M3 implementation is SDL3
// (src/runtime/platform/sdl, library dq8_platform_sdl, linked only by the host); presentation is a CPU copy
// of the scanout into a texture with an aspect fit. The full presentation pipeline of architecture §6.3 comes
// later. Implemented by M3 session D.
namespace dq8::rt {

struct WindowConfig {
    std::string title = "DQ8-Native";
    int width = 1280;
    int height = 960;
    bool vsync = true;
};

class IPlatform {
  public:
    virtual ~IPlatform() = default;
    // Processes window and input events; false once the user closed the window.
    virtual bool pump_events() = 0;
    // The live pad state for port 0 or 1 from the keyboard and gamepads (mapping fixed in M3).
    virtual PadState pad(unsigned port) const = 0;
    // Shows a frame scaled into the window; `aspect` is the display aspect (4/3 or 16/9).
    virtual void present(const gs::FrameImage& frame, double aspect) = 0;
};

// Throws std::runtime_error when no window can be created.
std::unique_ptr<IPlatform> create_sdl_platform(const WindowConfig& config);

} // namespace dq8::rt
