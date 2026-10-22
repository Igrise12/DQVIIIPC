// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "model/controllers.h"

#include "dq8/core/error.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dq8::launcher {

namespace {

struct ControlInfo {
    std::string_view key;
    std::string_view label;
};

// Session C's Control order (dq8/core/input_profile.h); the labels are ours.
constexpr std::array<ControlInfo, control_count> controls = {{
    {"up", "D-pad up"},
    {"down", "D-pad down"},
    {"left", "D-pad left"},
    {"right", "D-pad right"},
    {"triangle", "Triangle"},
    {"circle", "Circle"},
    {"cross", "Cross"},
    {"square", "Square"},
    {"l1", "L1"},
    {"r1", "R1"},
    {"l2", "L2"},
    {"r2", "R2"},
    {"l3", "L3 (press the left stick)"},
    {"r3", "R3 (press the right stick)"},
    {"start", "Start"},
    {"select", "Select"},
    {"left_stick_up", "Left stick up"},
    {"left_stick_down", "Left stick down"},
    {"left_stick_left", "Left stick left"},
    {"left_stick_right", "Left stick right"},
    {"right_stick_up", "Right stick up"},
    {"right_stick_down", "Right stick down"},
    {"right_stick_left", "Right stick left"},
    {"right_stick_right", "Right stick right"},
}};

constexpr float capture_threshold = 0.6f;

bool contains(const SourceList& list, std::string_view source) {
    return std::find(list.begin(), list.end(), source) != list.end();
}

void erase(SourceList& list, std::string_view source) {
    list.erase(std::remove(list.begin(), list.end(), source), list.end());
}

} // namespace

std::string_view control_key(std::size_t control) noexcept {
    return control < control_count ? controls[control].key : std::string_view();
}

std::string_view control_label(std::size_t control) noexcept {
    return control < control_count ? controls[control].label : std::string_view();
}

std::optional<std::size_t> control_from_key(std::string_view key) noexcept {
    for (std::size_t i = 0; i < control_count; ++i) {
        if (controls[i].key == key) {
            return i;
        }
    }
    return std::nullopt;
}

const DeviceEntry* InputProfile::device(std::string_view guid) const noexcept {
    for (const DeviceEntry& d : devices) {
        if (d.guid == guid) {
            return &d;
        }
    }
    return nullptr;
}

DeviceEntry* InputProfile::device(std::string_view guid) noexcept {
    for (DeviceEntry& d : devices) {
        if (d.guid == guid) {
            return &d;
        }
    }
    return nullptr;
}

bool valid_profile_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
               c == '-';
    });
}

InputProfile builtin_profile() {
    InputProfile p;
    const auto set = [](BindingTable& t, std::string_view key, std::initializer_list<const char*> sources) {
        SourceList& list = t.sources[*control_from_key(key)];
        list.assign(sources.begin(), sources.end());
    };
    // Keyboard: arrows for the d-pad, IJKL for the face buttons, as M3's table.
    set(p.keyboard, "up", {"Up"});
    set(p.keyboard, "down", {"Down"});
    set(p.keyboard, "left", {"Left"});
    set(p.keyboard, "right", {"Right"});
    set(p.keyboard, "triangle", {"I"});
    set(p.keyboard, "circle", {"L"});
    set(p.keyboard, "cross", {"K"});
    set(p.keyboard, "square", {"J"});
    set(p.keyboard, "l1", {"Q"});
    set(p.keyboard, "r1", {"E"});
    set(p.keyboard, "l2", {"1"});
    set(p.keyboard, "r2", {"3"});
    set(p.keyboard, "l3", {"2"});
    set(p.keyboard, "r3", {"4"});
    set(p.keyboard, "start", {"Return"});
    set(p.keyboard, "select", {"Backspace"});
    // Gamepads by position: the south face button is Cross.
    set(p.gamepad, "up", {"dpup"});
    set(p.gamepad, "down", {"dpdown"});
    set(p.gamepad, "left", {"dpleft"});
    set(p.gamepad, "right", {"dpright"});
    set(p.gamepad, "triangle", {"y"});
    set(p.gamepad, "circle", {"b"});
    set(p.gamepad, "cross", {"a"});
    set(p.gamepad, "square", {"x"});
    set(p.gamepad, "l1", {"leftshoulder"});
    set(p.gamepad, "r1", {"rightshoulder"});
    set(p.gamepad, "l2", {"lefttrigger"});
    set(p.gamepad, "r2", {"righttrigger"});
    set(p.gamepad, "l3", {"leftstick"});
    set(p.gamepad, "r3", {"rightstick"});
    set(p.gamepad, "start", {"start"});
    set(p.gamepad, "select", {"back"});
    set(p.gamepad, "left_stick_up", {"lefty-"});
    set(p.gamepad, "left_stick_down", {"lefty+"});
    set(p.gamepad, "left_stick_left", {"leftx-"});
    set(p.gamepad, "left_stick_right", {"leftx+"});
    set(p.gamepad, "right_stick_up", {"righty-"});
    set(p.gamepad, "right_stick_down", {"righty+"});
    set(p.gamepad, "right_stick_left", {"rightx-"});
    set(p.gamepad, "right_stick_right", {"rightx+"});
    return p;
}

std::vector<std::string> MemoryProfileStore::list() const {
    std::vector<std::string> names{"default"};
    for (const auto& [name, profile] : profiles_) {
        if (name != "default") {
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

InputProfile MemoryProfileStore::load(std::string_view name) const {
    const auto it = profiles_.find(name);
    return it != profiles_.end() ? it->second : builtin_profile();
}

void MemoryProfileStore::save(std::string_view name, const InputProfile& profile) {
    if (!valid_profile_name(name)) {
        throw core::Error(core::ErrorCode::settings_invalid,
                          "'" + std::string(name) + "' is not a profile name");
    }
    profiles_[std::string(name)] = profile;
    ++saves_;
}

void MemoryProfileStore::remove(std::string_view name) {
    const auto it = profiles_.find(name);
    if (it != profiles_.end()) {
        profiles_.erase(it);
    }
}

bool is_axis_name(std::string_view name) noexcept {
    return name == "leftx" || name == "lefty" || name == "rightx" || name == "righty" ||
           name == "lefttrigger" || name == "righttrigger";
}

std::optional<std::string> capture_source(const InputSignal& signal) {
    switch (signal.kind) {
    case InputSignal::Kind::key:
    case InputSignal::Kind::button:
        if (signal.value < 0.5f || signal.name.empty()) {
            return std::nullopt;
        }
        return signal.name;
    case InputSignal::Kind::axis:
        if (!is_axis_name(signal.name)) {
            return std::nullopt;
        }
        if (signal.name == "lefttrigger" || signal.name == "righttrigger") {
            return signal.value >= capture_threshold ? std::optional<std::string>(signal.name) : std::nullopt;
        }
        if (signal.value >= capture_threshold) {
            return signal.name + "+";
        }
        if (signal.value <= -capture_threshold) {
            return signal.name + "-";
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<AxisSource> parse_axis_source(std::string_view source) {
    int sign = 1;
    if (!source.empty() && (source.back() == '+' || source.back() == '-')) {
        sign = source.back() == '-' ? -1 : 1;
        source.remove_suffix(1);
    }
    if (!is_axis_name(source)) {
        return std::nullopt;
    }
    return AxisSource{std::string(source), sign};
}

ControllerModel::ControllerModel(ProfileStore& store, std::string profile_name) : store_(store) {
    if (!valid_profile_name(profile_name)) {
        profile_name = "default";
    }
    name_ = std::move(profile_name);
    try {
        profile_ = store_.load(name_);
    } catch (...) {
        problem_ = explain_current_exception();
        profile_ = store_.defaults();
    }
}

bool ControllerModel::select_profile(std::string_view name) {
    if (name == name_) {
        return true;
    }
    if (dirty_ && !save()) {
        return false;
    }
    try {
        profile_ = store_.load(name);
    } catch (...) {
        problem_ = explain_current_exception();
        return false;
    }
    name_ = std::string(name);
    cancel_capture();
    problem_.reset();
    status_ = "Profile " + name_ + " selected.";
    return true;
}

bool ControllerModel::create_profile(std::string_view name) {
    if (!valid_profile_name(name)) {
        problem_ = explain(core::ErrorCode::settings_invalid,
                           "a profile name is 1 to 64 letters, digits, '_' or '-'");
        return false;
    }
    const std::vector<std::string> existing = store_.list();
    if (std::find(existing.begin(), existing.end(), name) != existing.end()) {
        problem_ =
            explain(core::ErrorCode::settings_invalid, "a profile named " + std::string(name) + " exists");
        return false;
    }
    try {
        store_.save(name, profile_);
    } catch (...) {
        problem_ = explain_current_exception();
        return false;
    }
    if (dirty_) {
        save(); // the copy holds the unsaved edits; keep the original's too
    }
    name_ = std::string(name);
    dirty_ = false;
    problem_.reset();
    status_ = "Profile " + name_ + " created.";
    return true;
}

bool ControllerModel::delete_profile() {
    if (name_ == "default") {
        return false;
    }
    try {
        store_.remove(name_);
        profile_ = store_.load("default");
    } catch (...) {
        problem_ = explain_current_exception();
        return false;
    }
    status_ = "Profile " + name_ + " deleted.";
    name_ = "default";
    dirty_ = false;
    cancel_capture();
    return true;
}

void ControllerModel::set_connected(std::vector<ConnectedPad> pads) {
    const DeviceView before = current_device();
    connected_ = std::move(pads);
    // Keep the selection on the same device across a hot-plug.
    const std::vector<DeviceView> now = devices();
    selected_ = 0;
    for (std::size_t i = 0; i < now.size(); ++i) {
        if (now[i].kind == before.kind && now[i].guid == before.guid) {
            selected_ = i;
            break;
        }
    }
    if (capture_ && current_device().kind != before.kind) {
        cancel_capture();
    }
}

std::vector<DeviceView> ControllerModel::devices() const {
    std::vector<DeviceView> out;
    out.push_back({DeviceKind::keyboard, {}, "Keyboard", true, false, std::nullopt});
    out.push_back({DeviceKind::gamepads, {}, "All gamepads", !connected_.empty(), false, std::nullopt});
    const auto add = [&](const std::string& guid, const std::string& name, bool connected) {
        for (const DeviceView& d : out) {
            if (d.kind == DeviceKind::gamepad && d.guid == guid) {
                return;
            }
        }
        const DeviceEntry* entry = profile_.device(guid);
        DeviceView v{DeviceKind::gamepad,
                     guid,
                     name,
                     connected,
                     entry != nullptr,
                     entry != nullptr ? entry->port : std::nullopt};
        if (v.name.empty() && entry != nullptr) {
            v.name = entry->name;
        }
        out.push_back(std::move(v));
    };
    for (const ConnectedPad& pad : connected_) {
        add(pad.guid, pad.name, true);
    }
    for (const DeviceEntry& entry : profile_.devices) {
        add(entry.guid, entry.name, false);
    }
    return out;
}

void ControllerModel::select_device(std::size_t index) {
    const std::vector<DeviceView> all = devices();
    if (index < all.size() && index != selected_) {
        cancel_capture();
        selected_ = index;
    }
}

DeviceView ControllerModel::current_device() const {
    const std::vector<DeviceView> all = devices();
    return selected_ < all.size() ? all[selected_] : all.front();
}

void ControllerModel::customize_device() {
    const DeviceView d = current_device();
    if (d.kind != DeviceKind::gamepad || profile_.device(d.guid) != nullptr) {
        return;
    }
    DeviceEntry entry;
    entry.guid = d.guid;
    entry.name = d.name;
    profile_.devices.push_back(std::move(entry));
    status_ = d.name + " now has bindings of its own.";
    changed();
}

void ControllerModel::forget_device() {
    const DeviceView d = current_device();
    if (d.kind != DeviceKind::gamepad) {
        return;
    }
    auto& list = profile_.devices;
    const auto before = list.size();
    list.erase(
        std::remove_if(list.begin(), list.end(), [&](const DeviceEntry& e) { return e.guid == d.guid; }),
        list.end());
    if (list.size() != before) {
        status_ = d.name + " uses the bindings for all gamepads again.";
        changed();
        if (!d.connected) {
            selected_ = 1; // its row is gone
        }
    }
}

void ControllerModel::set_port(std::optional<unsigned> port) {
    const DeviceView d = current_device();
    if (d.kind != DeviceKind::gamepad || (port && *port > 1)) {
        return;
    }
    customize_device();
    status_.clear();
    DeviceEntry* entry = profile_.device(d.guid);
    if (entry == nullptr || entry->port == port) {
        return;
    }
    if (port) {
        for (DeviceEntry& other : profile_.devices) {
            if (&other != entry && other.port == port) {
                other.port.reset();
                status_ = other.name + " no longer has a fixed port. ";
            }
        }
    }
    entry->port = port;
    status_ += port ? d.name + " always takes port " + std::to_string(*port + 1) + "."
                    : d.name + " takes the first free port.";
    changed();
}

SourceList ControllerModel::sources(std::size_t control) const {
    if (control >= control_count) {
        return {};
    }
    const DeviceView d = current_device();
    switch (d.kind) {
    case DeviceKind::keyboard:
        return profile_.keyboard.sources[control];
    case DeviceKind::gamepads:
        return profile_.gamepad.sources[control];
    case DeviceKind::gamepad:
        if (const DeviceEntry* entry = profile_.device(d.guid);
            entry != nullptr && entry->overrides[control]) {
            return *entry->overrides[control];
        }
        return profile_.gamepad.sources[control];
    }
    return {};
}

bool ControllerModel::inherited(std::size_t control) const {
    const DeviceView d = current_device();
    if (d.kind != DeviceKind::gamepad || control >= control_count) {
        return false;
    }
    const DeviceEntry* entry = profile_.device(d.guid);
    return entry == nullptr || !entry->overrides[control];
}

SourceList& ControllerModel::editable(std::size_t control) {
    const DeviceView d = current_device();
    switch (d.kind) {
    case DeviceKind::keyboard:
        return profile_.keyboard.sources[control];
    case DeviceKind::gamepads:
        break;
    case DeviceKind::gamepad: {
        customize_device();
        DeviceEntry* entry = profile_.device(d.guid);
        if (!entry->overrides[control]) {
            entry->overrides[control] = profile_.gamepad.sources[control];
        }
        return *entry->overrides[control];
    }
    }
    return profile_.gamepad.sources[control];
}

void ControllerModel::remove_source(std::size_t control, std::string_view source) {
    if (control >= control_count || !contains(sources(control), source)) {
        return;
    }
    erase(editable(control), source);
    status_ = std::string(source) + " no longer drives " + std::string(control_label(control)) + ".";
    changed();
}

void ControllerModel::clear_control(std::size_t control) {
    if (control >= control_count) {
        return;
    }
    editable(control).clear();
    status_ = std::string(control_label(control)) + " is not bound.";
    changed();
}

void ControllerModel::reset_control(std::size_t control) {
    if (control >= control_count) {
        return;
    }
    const DeviceView d = current_device();
    switch (d.kind) {
    case DeviceKind::keyboard:
        profile_.keyboard.sources[control] = store_.defaults().keyboard.sources[control];
        break;
    case DeviceKind::gamepads:
        profile_.gamepad.sources[control] = store_.defaults().gamepad.sources[control];
        break;
    case DeviceKind::gamepad:
        if (DeviceEntry* entry = profile_.device(d.guid)) {
            entry->overrides[control].reset();
        }
        break;
    }
    status_ = std::string(control_label(control)) + " is back to its default.";
    changed();
}

void ControllerModel::reset_device() {
    const DeviceView d = current_device();
    switch (d.kind) {
    case DeviceKind::keyboard:
        profile_.keyboard = store_.defaults().keyboard;
        break;
    case DeviceKind::gamepads:
        profile_.gamepad = store_.defaults().gamepad;
        break;
    case DeviceKind::gamepad:
        if (DeviceEntry* entry = profile_.device(d.guid)) {
            entry->overrides = {};
        }
        break;
    }
    status_ = d.name + ": every control is back to its default.";
    changed();
}

void ControllerModel::begin_capture(std::size_t control, bool add, Clock::time_point now) {
    if (control >= control_count) {
        return;
    }
    capture_ = control;
    capture_add_ = add;
    capture_deadline_ = now + capture_timeout;
    status_.clear();
}

void ControllerModel::cancel_capture() {
    capture_.reset();
}

double ControllerModel::capture_seconds_left(Clock::time_point now) const {
    if (!capture_ || now >= capture_deadline_) {
        return 0.0;
    }
    return std::chrono::duration<double>(capture_deadline_ - now).count();
}

bool ControllerModel::accepts(const InputSignal& signal) const {
    const DeviceView d = current_device();
    switch (d.kind) {
    case DeviceKind::keyboard:
        return signal.kind == InputSignal::Kind::key;
    case DeviceKind::gamepads:
        return signal.kind != InputSignal::Kind::key;
    case DeviceKind::gamepad:
        return signal.kind != InputSignal::Kind::key && signal.device_guid == d.guid;
    }
    return false;
}

bool ControllerModel::on_signal(const InputSignal& signal, Clock::time_point now) {
    if (!capture_) {
        return false;
    }
    if (now >= capture_deadline_) {
        tick(now);
        return false;
    }
    if (signal.kind == InputSignal::Kind::key && signal.name == "Escape") {
        if (signal.value >= 0.5f) {
            cancel_capture();
            status_ = "Cancelled; the binding is unchanged.";
        }
        return true;
    }
    if (!accepts(signal)) {
        return true; // swallowed: another device's input must not reach the page while capturing
    }
    const std::optional<std::string> source = capture_source(signal);
    if (!source) {
        return true;
    }
    bind(*capture_, *source);
    capture_.reset();
    return true;
}

void ControllerModel::tick(Clock::time_point now) {
    if (capture_ && now >= capture_deadline_) {
        capture_.reset();
        status_ = "No input arrived; the binding is unchanged.";
    }
}

void ControllerModel::bind(std::size_t control, const std::string& source) {
    std::string moved;
    for (std::size_t other = 0; other < control_count; ++other) {
        if (other != control && contains(sources(other), source)) {
            erase(editable(other), source);
            moved = std::string(control_label(other));
        }
    }
    SourceList& list = editable(control);
    if (capture_add_) {
        if (!contains(list, source)) {
            list.push_back(source);
        }
    } else {
        list.assign(1, source);
    }
    status_ = source + " now drives " + std::string(control_label(control)) +
              (moved.empty() ? "." : " (moved from " + moved + ").");
    changed();
}

std::array<float, control_count>
ControllerModel::levels(const std::function<float(std::string_view source)>& level) const {
    std::array<float, control_count> out{};
    for (std::size_t c = 0; c < control_count; ++c) {
        float v = 0.0f;
        for (const std::string& s : sources(c)) {
            v = std::max(v, std::clamp(level(s), 0.0f, 1.0f));
        }
        out[c] = v;
    }
    return out;
}

void ControllerModel::changed() {
    dirty_ = true;
}

bool ControllerModel::save() {
    try {
        store_.save(name_, profile_);
    } catch (...) {
        problem_ = explain_current_exception();
        return false;
    }
    dirty_ = false;
    problem_.reset();
    return true;
}

} // namespace dq8::launcher
