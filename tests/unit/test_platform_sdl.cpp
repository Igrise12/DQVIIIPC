// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// The SDL3 platform (runtime/platform/sdl) on SDL's dummy video driver: no display needed. Covers the
// built-in input profile (M3's mapping), the aspect fit, keyboard events reaching PadState, presentation
// through the texture path (checked with a copy of the window), the window closing, and rumble reaching a
// (virtual) gamepad.

#include "runtime/gs/gs_backend.h"
#include "runtime/io/pad_state.h"
#include "runtime/platform/platform.h"
#include "runtime/platform/sdl/sdl_bindings.h"
#include "runtime/platform/sdl/sdl_mapping.h"
#include "runtime/platform/sdl/sdl_platform.h"

#include <SDL3/SDL.h>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace dq8::rt;
namespace gs = dq8::gs;
using Catch::Approx;

std::unique_ptr<IPlatform> dummy_platform() {
#if defined(_WIN32)
    _putenv_s("SDL_VIDEO_DRIVER", "dummy");
#else
    ::setenv("SDL_VIDEO_DRIVER", "dummy", 1);
#endif
    WindowConfig config;
    config.title = "dq8 test";
    config.width = 320;
    config.height = 240;
    config.vsync = false;
    return create_sdl_platform(config);
}

void push_key(SDL_Scancode code, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = code;
    event.key.down = down;
    REQUIRE(SDL_PushEvent(&event));
}

gs::FrameImage solid_frame(std::uint32_t w, std::uint32_t h, std::array<std::uint8_t, 4> rgba) {
    gs::FrameImage frame;
    frame.width = w;
    frame.height = h;
    frame.rgba.resize(std::size_t{w} * h * 4);
    for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
        for (std::size_t c = 0; c < 4; ++c) {
            frame.rgba[i + c] = rgba[c];
        }
    }
    return frame;
}

std::array<std::uint8_t, 4> pixel(const gs::FrameImage& image, std::uint32_t x, std::uint32_t y) {
    const std::uint8_t* p = image.rgba.data() + (std::size_t{y} * image.width + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

} // namespace

TEST_CASE("the built-in input profile is the M3 keyboard and gamepad table", "[platform_sdl]") {
    const dq8::core::input::Profile profile = dq8::core::input::default_profile();
    std::vector<std::string> unknown;
    const sdl::DeviceMap keys = sdl::keyboard_map(profile.keyboard, &unknown);
    const sdl::DeviceMap pad = sdl::gamepad_map(profile.gamepad, &unknown);
    CHECK(unknown.empty());

    // One key at a time, as M3's button_for_scancode() mapped it.
    const auto key_buttons = [&keys](SDL_Scancode down) {
        PadState state;
        sdl::apply_map(state, keys, [down](const sdl::Source& s) {
            return s.kind == sdl::Source::Kind::key && s.code == down ? 1.0f : 0.0f;
        });
        return state.buttons;
    };
    CHECK(key_buttons(SDL_SCANCODE_K) == pad_cross);
    CHECK(key_buttons(SDL_SCANCODE_L) == pad_circle);
    CHECK(key_buttons(SDL_SCANCODE_J) == pad_square);
    CHECK(key_buttons(SDL_SCANCODE_I) == pad_triangle);
    CHECK(key_buttons(SDL_SCANCODE_RETURN) == pad_start);
    CHECK(key_buttons(SDL_SCANCODE_BACKSPACE) == pad_select);
    CHECK(key_buttons(SDL_SCANCODE_UP) == pad_up);
    CHECK(key_buttons(SDL_SCANCODE_DOWN) == pad_down);
    CHECK(key_buttons(SDL_SCANCODE_Q) == pad_l1);
    CHECK(key_buttons(SDL_SCANCODE_E) == pad_r1);
    CHECK(key_buttons(SDL_SCANCODE_1) == pad_l2);
    CHECK(key_buttons(SDL_SCANCODE_3) == pad_r2);
    CHECK(key_buttons(SDL_SCANCODE_2) == pad_l3);
    CHECK(key_buttons(SDL_SCANCODE_4) == pad_r3);
    CHECK(key_buttons(SDL_SCANCODE_W) == 0); // a stick key, not a button
    CHECK(key_buttons(SDL_SCANCODE_ESCAPE) == 0);

    // One gamepad button at a time, as M3's button_for_gamepad_button() mapped it.
    const auto pad_buttons = [&pad](SDL_GamepadButton down) {
        sdl::GamepadReading reading;
        reading.buttons[static_cast<std::size_t>(down)] = true;
        PadState state;
        sdl::apply_map(state, pad, [&reading](const sdl::Source& s) { return reading.amount(s); });
        return state.buttons;
    };
    CHECK(pad_buttons(SDL_GAMEPAD_BUTTON_SOUTH) == pad_cross);
    CHECK(pad_buttons(SDL_GAMEPAD_BUTTON_EAST) == pad_circle);
    CHECK(pad_buttons(SDL_GAMEPAD_BUTTON_WEST) == pad_square);
    CHECK(pad_buttons(SDL_GAMEPAD_BUTTON_NORTH) == pad_triangle);
    CHECK(pad_buttons(SDL_GAMEPAD_BUTTON_START) == pad_start);
    CHECK(pad_buttons(SDL_GAMEPAD_BUTTON_BACK) == pad_select);
    CHECK(pad_buttons(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) == pad_l1);
    CHECK(pad_buttons(SDL_GAMEPAD_BUTTON_RIGHT_STICK) == pad_r3);
    CHECK(pad_buttons(SDL_GAMEPAD_BUTTON_DPAD_LEFT) == pad_left);
    CHECK(pad_buttons(SDL_GAMEPAD_BUTTON_GUIDE) == 0);

    // The triggers press L2 and R2 past a quarter; the sticks map straight through.
    sdl::GamepadReading reading;
    reading.axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = sdl::axis_amount(32767 / 4 + 200);
    reading.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = sdl::axis_amount(32767 / 4 - 200);
    reading.axes[SDL_GAMEPAD_AXIS_LEFTX] = sdl::axis_amount(-32768);
    reading.axes[SDL_GAMEPAD_AXIS_RIGHTY] = sdl::axis_amount(32767);
    PadState state;
    sdl::apply_map(state, pad, [&reading](const sdl::Source& s) { return reading.amount(s); });
    CHECK(state.buttons == pad_l2);
    CHECK(state.lx == 0x00);
    CHECK(state.ly == 0x80);
    CHECK(state.rx == 0x80);
    CHECK(state.ry == 0xFF);

    CHECK(sdl::stick_byte(-32768) == 0x00);
    CHECK(sdl::stick_byte(0) == 0x80);
    CHECK(sdl::stick_byte(32767) == 0xFF);
    CHECK(sdl::stick_byte_of(-1.0f) == 0x00);
    CHECK(sdl::stick_byte_of(0.0f) == 0x80);
    CHECK(sdl::stick_byte_of(1.0f) == 0xFF);

    PadState keyboard;
    sdl::apply_map(keyboard, keys, [](const sdl::Source& s) {
        const auto code = static_cast<SDL_Scancode>(s.code);
        return code == SDL_SCANCODE_K || code == SDL_SCANCODE_LEFT || code == SDL_SCANCODE_W ||
                       code == SDL_SCANCODE_H || code == SDL_SCANCODE_A || code == SDL_SCANCODE_D
                   ? 1.0f
                   : 0.0f;
    });
    CHECK(keyboard.buttons == (pad_cross | pad_left));
    CHECK(keyboard.ly == 0x00); // W: up
    CHECK(keyboard.lx == 0x80); // A and D cancel out
    CHECK(keyboard.rx == 0xFF); // H: right
    CHECK(keyboard.ry == 0x80);
    sdl::fill_pressure(keyboard);
    // Report order: right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2.
    CHECK(keyboard.pressure == std::array<std::uint8_t, 12>{0, 0xFF, 0, 0, 0, 0, 0xFF, 0, 0, 0, 0, 0});
}

TEST_CASE("aspect_fit centres the largest 4:3 or 16:9 rectangle", "[platform_sdl]") {
    const sdl::FitRect pillar = sdl::aspect_fit(1920, 1080, 4.0 / 3.0);
    CHECK(pillar.w == Approx(1440));
    CHECK(pillar.h == Approx(1080));
    CHECK(pillar.x == Approx(240));
    CHECK(pillar.y == Approx(0));

    const sdl::FitRect letterbox = sdl::aspect_fit(1280, 960, 16.0 / 9.0);
    CHECK(letterbox.w == Approx(1280));
    CHECK(letterbox.h == Approx(720));
    CHECK(letterbox.x == Approx(0));
    CHECK(letterbox.y == Approx(120));

    const sdl::FitRect own = sdl::aspect_fit(800, 600, 0.0);
    CHECK(own.w == Approx(800));
    CHECK(own.h == Approx(600));

    const sdl::FitRect none = sdl::aspect_fit(0, 600, 4.0 / 3.0);
    CHECK(none.w == Approx(0.0f));
}

TEST_CASE("the SDL platform runs on the dummy video driver", "[platform_sdl]") {
    auto platform = dummy_platform();
    REQUIRE(platform != nullptr);
    CHECK(platform->pump_events());

    SECTION("keyboard events reach pad port 0") {
        const PadState idle = platform->pad(0);
        CHECK(idle.connected);
        CHECK(idle.buttons == 0);
        CHECK(idle.lx == 0x80);

        push_key(SDL_SCANCODE_K, true);
        push_key(SDL_SCANCODE_DOWN, true);
        CHECK(platform->pump_events());
        const PadState pressed = platform->pad(0);
        CHECK(pressed.buttons == (pad_cross | pad_down));
        CHECK(pressed.pressure[3] == 0xFF); // down
        CHECK(pressed.pressure[6] == 0xFF); // cross

        push_key(SDL_SCANCODE_K, false);
        CHECK(platform->pump_events());
        CHECK(platform->pad(0).buttons == pad_down);
    }

    SECTION("present draws the frame with the aspect fit") {
        const gs::FrameImage red = solid_frame(64, 48, {255, 0, 0, 255});
        platform->present(red, 4.0 / 3.0);

        // 16:9 in a 320x240 window: 320x180 with 30-pixel bars above and below.
        REQUIRE(sdl::request_window_capture(*platform));
        platform->present(red, 16.0 / 9.0);
        gs::FrameImage shot;
        REQUIRE(sdl::take_window_capture(*platform, shot));
        REQUIRE(shot.width == 320);
        REQUIRE(shot.height == 240);
        const auto centre = pixel(shot, 160, 120);
        CHECK(centre[0] == 255);
        CHECK(centre[1] == 0);
        CHECK(centre[2] == 0);
        const auto bar = pixel(shot, 160, 10);
        CHECK(bar[0] == 0);
        CHECK(bar[1] == 0);
        CHECK(bar[2] == 0);

        // An empty frame shows the last one again.
        REQUIRE(sdl::request_window_capture(*platform));
        platform->present(gs::FrameImage{}, 4.0 / 3.0);
        REQUIRE(sdl::take_window_capture(*platform, shot));
        CHECK(pixel(shot, 160, 120)[0] == 255);
        CHECK_FALSE(sdl::take_window_capture(*platform, shot)); // one capture per request
    }

    SECTION("closing the window ends pump_events") {
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        REQUIRE(SDL_PushEvent(&quit));
        CHECK_FALSE(platform->pump_events());
    }
}

TEST_CASE("a second SDL platform can follow the first", "[platform_sdl]") {
    {
        auto first = dummy_platform();
    }
    auto second = dummy_platform();
    CHECK(second->pump_events());
}

TEST_CASE("set_rumble drives the port's gamepad through SDL", "[platform][sdl]") {
    auto platform = dummy_platform();
    // A gamepad already connected to the host takes port 0 first, and rumble would go to it.
    platform->pump_events();
    int present = 0;
    if (SDL_JoystickID* pads = SDL_GetGamepads(&present)) {
        SDL_free(pads);
    }
    if (present > 0) {
        SKIP("a gamepad is connected to this host, so port 0 is not the virtual pad");
    }
    struct Seen {
        int calls = 0;
        Uint16 low = 0;
        Uint16 high = 0;
    } seen;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "dq8 virtual pad";
    desc.userdata = &seen;
    desc.Rumble = [](void* userdata, Uint16 low, Uint16 high) {
        auto* s = static_cast<Seen*>(userdata);
        ++s->calls;
        s->low = low;
        s->high = high;
        return true;
    };
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (id == 0) {
        SKIP("no virtual joystick: " << SDL_GetError());
    }
    platform->pump_events(); // the gamepad arrives as port 0
    platform->set_rumble(0, 0x8080, 0xFFFF);
    CHECK(seen.calls >= 1);
    CHECK(seen.low == 0x8080);
    CHECK(seen.high == 0xFFFF);
    platform->set_rumble(0, 0, 0);
    CHECK(seen.low == 0);
    CHECK(seen.high == 0);
    platform->set_rumble(1, 0x1000, 0); // no gamepad on port 1: nothing to drive, nothing fails
    SDL_DetachVirtualJoystick(id);
    platform->pump_events();
}
