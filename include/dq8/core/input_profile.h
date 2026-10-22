// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Input profiles (architecture §8.1, §9.3; M5 session C): which keys and gamepad controls drive which
// DualShock 2 control, stored per profile in user_config_dir()/input/<name>.toml. The format is
// Docs/formats/input-profile.md. This file only reads and writes the data: sources are SDL3's own names,
// kept as strings, so that dq8-game (src/runtime/platform/sdl) and the launcher both turn them into SDL codes
// with SDL itself. Errors are Error(settings_parse) for a file that is not TOML and Error(settings_invalid)
// for one that breaks the format, with "<file>:<line>:" first.
namespace dq8::core::input {

inline constexpr int profile_schema_version = 1;

// The DualShock 2 controls a source can drive. Stick directions are half axes: a digital source gives full
// deflection, an analog one its own amount.
enum class Control : std::uint8_t {
    up,
    down,
    left,
    right,
    triangle,
    circle,
    cross,
    square,
    l1,
    r1,
    l2,
    r2,
    l3,
    r3,
    start,
    select,
    left_stick_up,
    left_stick_down,
    left_stick_left,
    left_stick_right,
    right_stick_up,
    right_stick_down,
    right_stick_left,
    right_stick_right,
};
inline constexpr std::size_t control_count = 24;

// "up", "cross", "left_stick_up", ... (the keys of the file).
std::string_view control_name(Control control) noexcept;
std::optional<Control> control_from_name(std::string_view name) noexcept;
inline constexpr Control control_at(std::size_t index) noexcept {
    return static_cast<Control>(index);
}

// The sources bound to each control on one kind of device, in the file's order. A key is named as
// SDL_GetScancodeName() prints it ("K", "Return", "Left"); a gamepad button as
// SDL_GetGamepadStringForButton()
// ("a" is the south face button, by position), a gamepad axis as SDL_GetGamepadStringForAxis() with an
// optional sign choosing one half ("leftx-", "lefty+"; no sign is the positive half, as for the triggers).
struct Bindings {
    std::array<std::vector<std::string>, control_count> sources;

    std::vector<std::string>& operator[](Control c) { return sources[static_cast<std::size_t>(c)]; }
    const std::vector<std::string>& operator[](Control c) const {
        return sources[static_cast<std::size_t>(c)];
    }
    friend bool operator==(const Bindings&, const Bindings&) = default;
};

// One particular gamepad: its own bindings (a control it does not list uses [gamepad]'s) and, optionally, the
// pad port it always takes.
struct DeviceProfile {
    std::string guid; // SDL_GUIDToString(): 32 lowercase hex digits
    std::string name; // for people (SDL_GetGamepadName() when it was written); never used to match
    std::optional<unsigned> port; // 0 or 1 (the file says 1 or 2, as players count pad ports)
    // Only the controls the file lists for this device; see effective().
    std::array<std::optional<std::vector<std::string>>, control_count> overrides;
    friend bool operator==(const DeviceProfile&, const DeviceProfile&) = default;
};

struct Profile {
    Bindings keyboard; // pad port 0
    Bindings gamepad;  // every gamepad without a device entry of its own
    std::vector<DeviceProfile> devices;

    // The device entry for `guid`, or null.
    const DeviceProfile* device(std::string_view guid) const noexcept;
    // The bindings a gamepad with this GUID uses: [gamepad], with the device's own entries over it.
    Bindings gamepad_for(std::string_view guid) const;
    friend bool operator==(const Profile&, const Profile&) = default;
};

// The built-in bindings (M3's fixed table, src/runtime/platform/sdl/sdl_mapping.h).
Profile default_profile();

// A file's text. `origin` names it in errors. A missing [keyboard] or [gamepad] table, or a control a table
// does not list, keeps the built-in bindings; an empty list unbinds the control.
Profile parse_profile(std::string_view text, std::string_view origin);
// Reads a file (Error(io_open) when it is missing).
Profile load_profile(const std::filesystem::path& file);
// The text of a whole profile: every control of both tables, then the devices; parse_profile() of it gives
// `profile` back.
std::string format_profile(const Profile& profile);
// Writes format_profile() atomically, creating the directory.
void save_profile(const std::filesystem::path& file, const Profile& profile);

// A profile name: 1 to 64 of A-Z, a-z, 0-9, '_' and '-'.
bool valid_profile_name(std::string_view name) noexcept;
// Where profile `name` lives: <config_dir>/input/<name>.toml.
std::filesystem::path profile_file(const std::filesystem::path& config_dir, std::string_view name);

} // namespace dq8::core::input
