// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/platform/platform.h"

// Extras of the SDL3 platform that are not part of IPlatform: a copy of what the window shows, for tools and
// tests (tools/gs/dq8-gs-replay --window-capture). Main thread only, like the platform.
namespace dq8::rt::sdl {

// Asks a platform made by create_sdl_platform() to keep a copy of the window contents at its next present().
// False for any other IPlatform.
bool request_window_capture(IPlatform& platform);
// The copy taken by the last present() after request_window_capture(), as RGBA8; false when there is none.
bool take_window_capture(IPlatform& platform, gs::FrameImage& out);

} // namespace dq8::rt::sdl
