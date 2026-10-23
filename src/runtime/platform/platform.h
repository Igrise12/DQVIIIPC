// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/input_profile.h"
#include "runtime/gs/gs_backend.h"
#include "runtime/io/pad_state.h"

#include <cstdint>
#include <memory>
#include <string>

// The window, input and presentation of dq8-game (ADR-0005). Main thread only. The M3 implementation is SDL3
// (src/runtime/platform/sdl, library dq8_platform_sdl, linked only by the host); presentation is a CPU copy
// of the scanout into a texture with an aspect fit. The full presentation pipeline of architecture §6.3 comes
// later. Implemented by M3 session D; window modes, scaling options and input profiles by M5 session C.
namespace dq8::rt {

// M5 session C: window modes, scaling and the input configuration (Docs/milestones/M5-launcher.md).
enum class WindowMode { windowed, borderless, fullscreen }; // fullscreen: exclusive, at the desktop's mode
enum class ScaleFilter { linear, nearest };

// The defaults are M4's behaviour: the M3 bindings, no dead zone, full rumble.
struct InputConfig {
    core::input::Profile profile = core::input::default_profile(); // keyboard and gamepad bindings
    std::uint32_t deadzone_percent = 0;                            // radial, on each gamepad stick
    std::uint32_t rumble_percent = 100;                            // scales what set_rumble asks for
};

struct WindowConfig {
    std::string title = "DQ8-Native";
    int width = 1280; // the windowed size (also what Alt+Enter returns to)
    int height = 960;
    bool vsync = true;
    WindowMode mode = WindowMode::windowed;
    ScaleFilter filter = ScaleFilter::linear;
    bool integer_scaling = false; // whole-number vertical scale factors (both axes with aspect 0)
    InputConfig input;
};

class IPlatform {
  public:
    virtual ~IPlatform() = default;
    // Processes window and input events; false once the user closed the window.
    virtual bool pump_events() = 0;
    // The live pad state for port 0 or 1: the keyboard drives port 0, each gamepad the port it was given
    // (bindings from WindowConfig::input).
    virtual PadState pad(unsigned port) const = 0;
    // Shows a frame scaled into the window; `aspect` is the display aspect (4/3 or 16/9), or 0 to fill the
    // window (stretch).
    virtual void present(const gs::FrameImage& frame, double aspect) = 0;
    // Drives the gamepad motors of `port` (M4 session D): low and high frequency strengths, 0 = off.
    virtual void set_rumble(unsigned /*port*/, std::uint16_t /*low*/, std::uint16_t /*high*/) {}
};

// Throws std::runtime_error when no window can be created.
std::unique_ptr<IPlatform> create_sdl_platform(const WindowConfig& config);

} // namespace dq8::rt
