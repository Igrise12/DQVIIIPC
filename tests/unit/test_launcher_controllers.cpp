// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The Controllers page's model: profiles, devices, live capture and the test view, over the in-memory store
// that stands in until session C's profile files land.
#include "model/controllers.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <set>
#include <string>

using namespace dq8::launcher;
using namespace std::chrono_literals;
using Kind = InputSignal::Kind;

namespace {

std::size_t control(std::string_view key) {
    const auto c = control_from_key(key);
    REQUIRE(c);
    return *c;
}

InputSignal key(std::string name, float value = 1.0f) {
    return {Kind::key, {}, std::move(name), value};
}

InputSignal button(std::string guid, std::string name, float value = 1.0f) {
    return {Kind::button, std::move(guid), std::move(name), value};
}

InputSignal axis(std::string guid, std::string name, float value) {
    return {Kind::axis, std::move(guid), std::move(name), value};
}

const auto t0 = std::chrono::steady_clock::time_point(std::chrono::seconds(50));

constexpr const char* pad_a = "030000005e0400008e02000010010000";
constexpr const char* pad_b = "03000000de2800000512000011010000";

} // namespace

TEST_CASE("launcher: the 24 controls in session C's order", "[launcher][controllers]") {
    CHECK(control_key(0) == "up");
    CHECK(control_key(6) == "cross");
    CHECK(control_key(16) == "left_stick_up");
    CHECK(control_key(23) == "right_stick_right");
    CHECK(control_key(24).empty());
    std::set<std::string_view> keys;
    for (std::size_t c = 0; c < control_count; ++c) {
        keys.insert(control_key(c));
        CHECK_FALSE(control_label(c).empty());
        CHECK(control_from_key(control_key(c)) == c);
    }
    CHECK(keys.size() == control_count);
    CHECK_FALSE(control_from_key("x"));
}

TEST_CASE("launcher: the built-in profile is M3's table", "[launcher][controllers]") {
    const InputProfile p = builtin_profile();
    CHECK(p.keyboard.sources[control("cross")] == SourceList{"K"});
    CHECK(p.keyboard.sources[control("start")] == SourceList{"Return"});
    CHECK(p.keyboard.sources[control("left_stick_up")].empty());
    CHECK(p.gamepad.sources[control("cross")] == SourceList{"a"});
    CHECK(p.gamepad.sources[control("l2")] == SourceList{"lefttrigger"});
    CHECK(p.gamepad.sources[control("left_stick_left")] == SourceList{"leftx-"});
    for (std::size_t c = 0; c < control_count; ++c) {
        CHECK_FALSE(p.gamepad.sources[c].empty()); // every control reachable from a gamepad
    }
    CHECK(p.devices.empty());
}

TEST_CASE("launcher: capture sources and axis names", "[launcher][controllers]") {
    CHECK(capture_source(key("K")) == "K");
    CHECK_FALSE(capture_source(key("K", 0.0f))); // a release
    CHECK(capture_source(button(pad_a, "south")) == "south");
    CHECK(capture_source(axis(pad_a, "leftx", -0.9f)) == "leftx-");
    CHECK(capture_source(axis(pad_a, "righty", 0.7f)) == "righty+");
    CHECK_FALSE(capture_source(axis(pad_a, "leftx", 0.3f))); // too small
    CHECK(capture_source(axis(pad_a, "lefttrigger", 0.8f)) == "lefttrigger");
    CHECK_FALSE(capture_source(axis(pad_a, "lefttrigger", -1.0f)));
    CHECK_FALSE(capture_source(axis(pad_a, "wheel", 1.0f)));

    const auto a = parse_axis_source("leftx-");
    REQUIRE(a);
    CHECK(a->axis == "leftx");
    CHECK(a->sign == -1);
    CHECK(parse_axis_source("righttrigger")->sign == 1);
    CHECK_FALSE(parse_axis_source("a"));
    CHECK_FALSE(parse_axis_source("K"));
}

TEST_CASE("launcher: capturing a key binding", "[launcher][controllers]") {
    MemoryProfileStore store;
    ControllerModel m(store, "default");
    CHECK(m.current_device().kind == DeviceKind::keyboard);
    CHECK_FALSE(m.on_signal(key("X"), t0)); // not capturing: not consumed

    m.begin_capture(control("cross"), false, t0);
    REQUIRE(m.capturing());
    CHECK(m.capture_seconds_left(t0 + 1s) == 5.0);
    CHECK(m.on_signal(button(pad_a, "a"), t0 + 1s)); // a gamepad: swallowed, not bound
    CHECK(m.capturing());
    CHECK(m.on_signal(key("Space"), t0 + 2s));
    CHECK_FALSE(m.capturing());
    CHECK(m.sources(control("cross")) == SourceList{"Space"});
    CHECK(m.dirty());
    CHECK(m.status().find("Space") != std::string::npos);

    // Binding a key that drives another control moves it.
    m.begin_capture(control("circle"), false, t0);
    m.on_signal(key("Space"), t0);
    CHECK(m.sources(control("circle")) == SourceList{"Space"});
    CHECK(m.sources(control("cross")).empty());
    CHECK(m.status().find("moved from Cross") != std::string::npos);

    // Add beside the existing source.
    m.begin_capture(control("circle"), true, t0);
    m.on_signal(key("L"), t0);
    CHECK(m.sources(control("circle")) == SourceList{"Space", "L"});

    // Escape cancels; a timeout leaves the binding alone.
    m.begin_capture(control("square"), false, t0);
    CHECK(m.on_signal(key("Escape"), t0));
    CHECK_FALSE(m.capturing());
    CHECK(m.sources(control("square")) == SourceList{"J"});
    m.begin_capture(control("square"), false, t0);
    m.tick(t0 + 7s);
    CHECK_FALSE(m.capturing());
    CHECK(m.status().find("No input") != std::string::npos);

    REQUIRE(m.save());
    CHECK(store.saves() == 1);
    CHECK_FALSE(m.dirty());
    CHECK(store.load("default").keyboard.sources[control("circle")] == SourceList{"Space", "L"});
}

TEST_CASE("launcher: gamepad tables and per-device entries", "[launcher][controllers]") {
    MemoryProfileStore store;
    ControllerModel m(store, "default");
    m.set_connected({{pad_a, "Pad A"}, {pad_b, "Pad B"}});
    const auto devices = m.devices();
    REQUIRE(devices.size() == 4);
    CHECK(devices[1].kind == DeviceKind::gamepads);
    CHECK(devices[1].connected);
    CHECK(devices[2].guid == pad_a);
    CHECK_FALSE(devices[2].customized);

    // All gamepads: any pad's input binds.
    m.select_device(1);
    m.begin_capture(control("cross"), false, t0);
    CHECK(m.on_signal(key("K"), t0)); // keyboard input is swallowed here
    CHECK(m.capturing());
    m.on_signal(button(pad_b, "b"), t0);
    CHECK(m.sources(control("cross")) == SourceList{"b"});
    CHECK(m.sources(control("circle")).empty()); // "b" moved from Circle

    // Pad A: inherits until edited; only pad A's input binds.
    m.select_device(2);
    CHECK(m.inherited(control("cross")));
    CHECK(m.sources(control("cross")) == SourceList{"b"});
    m.begin_capture(control("cross"), false, t0);
    CHECK(m.on_signal(button(pad_b, "a"), t0)); // the other pad: swallowed
    CHECK(m.capturing());
    m.on_signal(axis(pad_a, "lefttrigger", 1.0f), t0);
    CHECK(m.sources(control("cross")) == SourceList{"lefttrigger"});
    CHECK_FALSE(m.inherited(control("cross")));
    CHECK(m.current_device().customized);
    // lefttrigger drove L2 through the gamepad table: on pad A it moved, so L2 is now pad A's own (empty)
    // list.
    CHECK(m.sources(control("l2")).empty());
    CHECK_FALSE(m.inherited(control("l2")));
    const DeviceEntry* entry = m.profile().device(pad_a);
    REQUIRE(entry);
    CHECK(entry->name == "Pad A");
    CHECK(m.profile().gamepad.sources[control("l2")] == SourceList{"lefttrigger"}); // the table is untouched

    m.reset_control(control("cross"));
    CHECK(m.inherited(control("cross")));
    m.reset_device();
    CHECK(m.inherited(control("l2")));

    // Ports: two pads cannot both hold port 1.
    m.set_port(0u);
    CHECK(m.current_device().port == 0u);
    m.select_device(3);
    m.set_port(0u);
    CHECK(m.current_device().port == 0u);
    CHECK(m.profile().device(pad_a)->port == std::nullopt);
    m.set_port(5u); // not a port
    CHECK(m.current_device().port == 0u);

    // A remembered pad that is unplugged stays listed; the selection follows the device across a hot-plug.
    m.set_connected({{pad_b, "Pad B"}});
    CHECK(m.current_device().guid == pad_b);
    const auto after = m.devices();
    REQUIRE(after.size() == 4);
    CHECK(after[3].guid == pad_a);
    CHECK_FALSE(after[3].connected);
    m.select_device(3);
    m.forget_device();
    CHECK(m.devices().size() == 3);
    CHECK(m.current_device().kind == DeviceKind::gamepads);
}

TEST_CASE("launcher: removing, clearing and resetting bindings", "[launcher][controllers]") {
    MemoryProfileStore store;
    ControllerModel m(store, "default");
    m.remove_source(control("cross"), "K");
    CHECK(m.sources(control("cross")).empty());
    m.remove_source(control("cross"), "K"); // already gone: no change
    m.reset_control(control("cross"));
    CHECK(m.sources(control("cross")) == SourceList{"K"});
    m.clear_control(control("start"));
    CHECK(m.sources(control("start")).empty());
    m.reset_device();
    CHECK(m.profile().keyboard == builtin_profile().keyboard);
}

TEST_CASE("launcher: the test view's levels", "[launcher][controllers]") {
    MemoryProfileStore store;
    ControllerModel m(store, "default");
    m.select_device(1); // all gamepads
    const auto levels = m.levels([](std::string_view source) -> float {
        if (source == "a") {
            return 1.0f;
        }
        if (source == "leftx-") {
            return 0.4f;
        }
        if (source == "righttrigger") {
            return 7.0f; // clamped
        }
        return 0.0f;
    });
    CHECK(levels[control("cross")] == 1.0f);
    CHECK(levels[control("left_stick_left")] == 0.4f);
    CHECK(levels[control("r2")] == 1.0f);
    CHECK(levels[control("circle")] == 0.0f);
}

TEST_CASE("launcher: input profiles", "[launcher][controllers]") {
    MemoryProfileStore store;
    ControllerModel m(store, "no spaces allowed");
    CHECK(m.profile_name() == "default");
    CHECK(m.profiles() == std::vector<std::string>{"default"});
    CHECK_FALSE(m.persistent());

    m.clear_control(control("start"));
    REQUIRE(m.create_profile("couch"));
    CHECK(m.profile_name() == "couch");
    CHECK(m.profiles() == std::vector<std::string>{"couch", "default"});
    CHECK(m.sources(control("start")).empty()); // a copy of the edited profile
    CHECK_FALSE(m.create_profile("couch"));
    CHECK_FALSE(m.create_profile("bad name"));
    REQUIRE(m.problem());

    REQUIRE(m.select_profile("default"));
    CHECK(m.profile_name() == "default");
    CHECK_FALSE(m.delete_profile()); // the default stays
    REQUIRE(m.select_profile("couch"));
    REQUIRE(m.delete_profile());
    CHECK(m.profile_name() == "default");
    CHECK(m.profiles() == std::vector<std::string>{"default"});

    CHECK(valid_profile_name("a-b_C9"));
    CHECK_FALSE(valid_profile_name(""));
    CHECK_FALSE(valid_profile_name(std::string(65, 'a')));
    CHECK_FALSE(valid_profile_name("../x"));
}
