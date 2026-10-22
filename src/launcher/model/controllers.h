// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "model/errors.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The Controllers page (architecture.md §11.2): input profiles, the bindings of each device, live capture of
// a binding and a test view. A profile binds the DualShock 2's 24 controls to SDL3's own names for keys
// ("K", "Return"), gamepad buttons ("a", "dpup") and half axes ("leftx-", "lefttrigger"), for the keyboard,
// for every gamepad, and per gamepad by GUID with an optional fixed port. The files are session C's
// (user_config_dir()/input/<name>.toml, Docs/formats/); the page reaches them only through ProfileStore, and
// the types here mirror C's, control for control, so the store over C's API is a field-by-field copy.
namespace dq8::launcher {

inline constexpr std::size_t control_count = 24;

// "up", "cross", "left_stick_up", ...: the keys of C's profile files, in C's order.
std::string_view control_key(std::size_t control) noexcept;
// "D-pad up", "Cross", "Left stick up", ...
std::string_view control_label(std::size_t control) noexcept;
std::optional<std::size_t> control_from_key(std::string_view key) noexcept;

using SourceList = std::vector<std::string>;

struct BindingTable {
    std::array<SourceList, control_count> sources;
    friend bool operator==(const BindingTable&, const BindingTable&) = default;
};

struct DeviceEntry {
    std::string guid; // SDL's GUID string, 32 lowercase hex digits
    std::string name; // for people
    std::optional<unsigned> port;
    std::array<std::optional<SourceList>, control_count> overrides; // unset: the gamepad table's
    friend bool operator==(const DeviceEntry&, const DeviceEntry&) = default;
};

struct InputProfile {
    BindingTable keyboard;
    BindingTable gamepad;
    std::vector<DeviceEntry> devices;
    friend bool operator==(const InputProfile&, const InputProfile&) = default;

    const DeviceEntry* device(std::string_view guid) const noexcept;
    DeviceEntry* device(std::string_view guid) noexcept;
};

// 1 to 64 of A-Z, a-z, 0-9, '_' and '-' (C's rule).
bool valid_profile_name(std::string_view name) noexcept;

class ProfileStore {
  public:
    virtual ~ProfileStore() = default;
    virtual std::vector<std::string> list() const = 0; // sorted; "default" always among them
    // The profile's bindings; a profile without a file is the built-in default.
    virtual InputProfile load(std::string_view name) const = 0;
    virtual void save(std::string_view name, const InputProfile& profile) = 0; // throws core::Error
    virtual void remove(std::string_view name) = 0;
    virtual InputProfile defaults() const = 0;
    // False for MemoryProfileStore: the page says that changes last until the launcher closes.
    virtual bool persistent() const = 0;
};

// M3's fixed bindings (src/runtime/platform/sdl/sdl_mapping.cpp) as a profile, until session C's
// default_profile() replaces this copy.
InputProfile builtin_profile();

// Profiles in memory: the tests, the preview, and the page until C's profile files land.
class MemoryProfileStore final : public ProfileStore {
  public:
    std::vector<std::string> list() const override;
    InputProfile load(std::string_view name) const override;
    void save(std::string_view name, const InputProfile& profile) override;
    void remove(std::string_view name) override;
    InputProfile defaults() const override { return builtin_profile(); }
    bool persistent() const override { return false; }

    int saves() const noexcept { return saves_; }

  private:
    std::map<std::string, InputProfile, std::less<>> profiles_;
    int saves_ = 0;
};

enum class DeviceKind {
    keyboard, // the [keyboard] table (pad port 0)
    gamepads, // the [gamepad] table: every gamepad without an entry of its own
    gamepad,  // one gamepad by GUID: its entry's overrides over [gamepad]
};

struct DeviceView {
    DeviceKind kind = DeviceKind::keyboard;
    std::string guid;
    std::string name;
    bool connected = false;
    bool customized = false; // gamepad: the profile has an entry for it
    std::optional<unsigned> port;
};

struct ConnectedPad {
    std::string guid;
    std::string name;
};

// One input as SDL reported it, already in SDL's names.
struct InputSignal {
    enum class Kind { key, button, axis };
    Kind kind = Kind::key;
    std::string device_guid; // button, axis
    std::string name;   // SDL_GetScancodeName, SDL_GetGamepadStringForButton, SDL_GetGamepadStringForAxis
    float value = 1.0f; // axis: -1 to 1; key, button: 1 pressed, 0 released
};

// The source a capture binds for a signal: a key or button when pressed, an axis past half way with its sign
// ("leftx-"), a trigger without one ("lefttrigger"). Nullopt for releases and small movements.
std::optional<std::string> capture_source(const InputSignal& signal);

struct AxisSource {
    std::string axis; // "leftx"
    int sign = 1;     // the half: -1 or +1
};
// "leftx-" -> {leftx, -1}, "lefttrigger" -> {lefttrigger, +1}; nullopt for names that are not axes.
std::optional<AxisSource> parse_axis_source(std::string_view source);
bool is_axis_name(std::string_view name) noexcept;

class ControllerModel {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds capture_timeout{6};

    ControllerModel(ProfileStore& store, std::string profile_name);

    // Profiles.
    std::vector<std::string> profiles() const { return store_.list(); }
    const std::string& profile_name() const noexcept { return name_; }
    const InputProfile& profile() const noexcept { return profile_; }
    bool select_profile(std::string_view name);
    bool create_profile(std::string_view name); // a copy of the current one, then selected
    bool delete_profile();                      // not "default"; selects "default"
    bool persistent() const { return store_.persistent(); }

    // Devices: the keyboard, all gamepads, then each connected gamepad and each one the profile remembers.
    void set_connected(std::vector<ConnectedPad> pads);
    std::vector<DeviceView> devices() const;
    std::size_t selected_device() const noexcept { return selected_; }
    void select_device(std::size_t index);
    DeviceView current_device() const;
    void customize_device(); // gamepad: give it an entry of its own
    void forget_device();    // gamepad: drop its entry (it uses the gamepad table again)
    void set_port(std::optional<unsigned> port);

    // The selected device's bindings.
    SourceList sources(std::size_t control) const;
    bool inherited(std::size_t control) const; // a gamepad without its own list for this control
    void remove_source(std::size_t control, std::string_view source);
    void clear_control(std::size_t control);
    void reset_control(std::size_t control); // the default (keyboard, gamepads) or inherit again (a gamepad)
    void reset_device();

    // Live capture: the next key (keyboard) or button or axis (gamepads) binds to `control`, replacing its
    // sources or, with `add`, beside them. Ends after capture_timeout without input.
    void begin_capture(std::size_t control, bool add, Clock::time_point now);
    void cancel_capture();
    std::optional<std::size_t> capturing() const noexcept { return capture_; }
    bool capture_adds() const noexcept { return capture_add_; }
    double capture_seconds_left(Clock::time_point now) const;
    // True when the signal was used (bound, or swallowed while capturing).
    bool on_signal(const InputSignal& signal, Clock::time_point now);
    void tick(Clock::time_point now);

    // Test view: each control's level (0 to 1) from the selected device's sources; `level` reads one source.
    std::array<float, control_count> levels(const std::function<float(std::string_view source)>& level) const;

    bool dirty() const noexcept { return dirty_; }
    bool save();
    const std::optional<Problem>& problem() const noexcept { return problem_; }
    const std::string& status() const noexcept { return status_; }

  private:
    bool accepts(const InputSignal& signal) const;
    void bind(std::size_t control, const std::string& source);
    SourceList& editable(std::size_t control); // creates a device override from the inherited list
    void changed();

    ProfileStore& store_;
    std::string name_;
    InputProfile profile_;
    std::vector<ConnectedPad> connected_;
    std::size_t selected_ = 0;
    std::optional<std::size_t> capture_;
    bool capture_add_ = false;
    Clock::time_point capture_deadline_{};
    bool dirty_ = false;
    std::optional<Problem> problem_;
    std::string status_;
};

} // namespace dq8::launcher
