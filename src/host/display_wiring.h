// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/input_profile.h"
#include "host/options.h"
#include "runtime/platform/platform.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dq8::game {
class DisplayWatcher;
}

// M5 session C: what the display, input and audio options of dq8-game (options.h) become on the host side:
// the window's configuration with the input profile, the aspect the window loop presents at, and the
// paraLLEl-GS options of --internal-resolution. Nothing here reaches the machine. Problems that leave a
// usable default (a missing or broken input profile) are returned as warnings, never thrown.
namespace dq8::host {

// What --internal-resolution N means for paraLLEl-GS (spike S5b): 1x is the native picture (M4's); 2x, 3x
// and 4x super-sample with 4, 8 and 16 samples per pixel and turn on high-resolution scan-out, super-sampled
// textures and back-buffer promotion, which together give a picture at twice the native resolution
// (paraLLEl-GS scans out at most twice the mode's size; more samples smooth it further).
struct InternalResolution {
    std::uint32_t super_sampling = 1;
    bool high_resolution = false;
};
InternalResolution internal_resolution(std::uint32_t factor) noexcept;

// The GPU pipeline cache: --pipeline-cache FILE, or none for "none". Without the option: the file
// parallel-gs.pipelines in the user data directory's cache/ with --window, none headless (an empty path).
std::filesystem::path pipeline_cache_file(const Options& o);

// Where --input-profile's profile is: a value ending in ".toml" or holding a directory separator is a file,
// anything else a profile name in <config_dir>/input/.
std::filesystem::path input_profile_file(const Options& o, const std::filesystem::path& config_dir);

// The bindings --input-profile names. A missing "default" profile is the built-in bindings without a warning;
// a missing other one, or a file that is not TOML, is the built-in bindings with a warning; entries the file
// cannot use are left out with a warning each (dq8/core/input_profile.h). Throws UsageError for a name that
// is not a valid profile name.
core::input::Profile load_input_profile(const Options& o, std::vector<std::string>& warnings);

// The window from the options and the input profile.
rt::WindowConfig window_config(const Options& o, std::vector<std::string>& warnings);

// The display aspect the window loop presents at: 4/3, 16/9, or 0 for "stretch" (the window's own). auto
// follows the game's option through `watcher`, 4:3 without one.
double present_aspect(const Options& o, const game::DisplayWatcher* watcher) noexcept;

} // namespace dq8::host
