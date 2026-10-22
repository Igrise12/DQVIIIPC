// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// M5 session C on SDL's dummy drivers (no display, no sound card): integer scaling and the filter, dead
// zones, custom bindings, port assignment across a replug (logic and virtual gamepads), window modes and
// Alt+Enter, rumble strength, and the audio sink's device choice and volume.

#include "dq8/core/input_profile.h"
#include "runtime/gs/gs_backend.h"
#include "runtime/io/pad_state.h"
#include "runtime/platform/audio.h"
#include "runtime/platform/platform.h"
#include "runtime/platform/sdl/sdl_audio.h"
#include "runtime/platform/sdl/sdl_bindings.h"
#include "runtime/platform/sdl/sdl_mapping.h"
#include "runtime/platform/sdl/sdl_platform.h"
#include "runtime/platform/sdl/sdl_ports.h"

#include <SDL3/SDL.h>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace dq8::rt;
namespace gs = dq8::gs;
namespace input = dq8::core::input;
using Catch::Approx;

void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

std::unique_ptr<IPlatform> dummy_platform(WindowConfig config = {}) {
    set_env("SDL_VIDEO_DRIVER", "dummy");
    config.title = "dq8 display test";
    config.width = 320;
    config.height = 240;
    config.vsync = false;
    return create_sdl_platform(config);
}

gs::FrameImage frame_of(std::uint32_t w, std::uint32_t h,
                        const std::vector<std::array<std::uint8_t, 4>>& px) {
    gs::FrameImage frame;
    frame.width = w;
    frame.height = h;
    frame.rgba.resize(std::size_t{w} * h * 4);
    for (std::size_t i = 0; i < std::size_t{w} * h; ++i) {
        for (std::size_t c = 0; c < 4; ++c) {
            frame.rgba[i * 4 + c] = px[i % px.size()][c];
        }
    }
    return frame;
}

std::array<std::uint8_t, 4> pixel(const gs::FrameImage& image, std::uint32_t x, std::uint32_t y) {
    const std::uint8_t* p = image.rgba.data() + (std::size_t{y} * image.width + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

gs::FrameImage capture(IPlatform& platform, const gs::FrameImage& frame, double aspect) {
    REQUIRE(sdl::request_window_capture(platform));
    platform.present(frame, aspect);
    gs::FrameImage shot;
    REQUIRE(sdl::take_window_capture(platform, shot));
    return shot;
}

bool host_has_gamepads() {
    int present = 0;
    if (SDL_JoystickID* pads = SDL_GetGamepads(&present)) {
        SDL_free(pads);
    }
    return present > 0;
}

// A virtual gamepad with its own vendor and product (and so its own GUID).
SDL_JoystickID attach_pad(std::uint16_t product, void* userdata = nullptr,
                          bool (*rumble)(void*, Uint16, Uint16) = nullptr) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x1234;
    desc.product_id = product;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "dq8 virtual pad";
    desc.userdata = userdata;
    desc.Rumble = rumble;
    return SDL_AttachVirtualJoystick(&desc);
}

} // namespace

TEST_CASE("present_rect: integer scaling by whole numbers, centred on whole pixels", "[display_sdl]") {
    // 512x448 at 4:3 in 1920x1080: the height scales by 2 (896), the width follows the aspect.
    const sdl::FitRect r = sdl::present_rect(1920, 1080, 512, 448, 4.0 / 3.0, true);
    CHECK(r.h == Approx(896));
    CHECK(r.w == Approx(1195)); // 896 * 4/3, rounded
    CHECK(r.x == Approx(362));
    CHECK(r.y == Approx(92));

    // 16:9 needs a narrower factor when the width runs out: 2 * 448 * 16/9 = 1593 > 1500, so 1.
    const sdl::FitRect wide = sdl::present_rect(1500, 1000, 512, 448, 16.0 / 9.0, true);
    CHECK(wide.h == Approx(448));
    CHECK(wide.w == Approx(796));

    // Stretch: each axis by its own whole number.
    const sdl::FitRect stretch = sdl::present_rect(1920, 1080, 512, 448, 0.0, true);
    CHECK(stretch.w == Approx(1536));
    CHECK(stretch.h == Approx(896));

    // Without integer scaling, or in a window smaller than the frame: the aspect fit.
    const sdl::FitRect fit = sdl::present_rect(1920, 1080, 512, 448, 4.0 / 3.0, false);
    CHECK(fit.h == Approx(1080));
    CHECK(fit.w == Approx(1440));
    const sdl::FitRect small = sdl::present_rect(320, 240, 512, 448, 4.0 / 3.0, true);
    CHECK(small.w == Approx(320));
    CHECK(small.h == Approx(240));
}

TEST_CASE("dead zone: radial, rescaled to full deflection", "[display_sdl]") {
    float x = 0.05f;
    float y = 0.05f;
    sdl::apply_deadzone(x, y, 0.08f); // magnitude 0.07: inside
    CHECK(x == 0.0f);
    CHECK(y == 0.0f);
    x = 1.0f;
    y = 0.0f;
    sdl::apply_deadzone(x, y, 0.08f);
    CHECK(x == Approx(1.0f));
    CHECK(y == 0.0f);
    x = 0.0f;
    y = -0.54f; // halfway between the edge of the zone and full deflection
    sdl::apply_deadzone(x, y, 0.08f);
    CHECK(y == Approx(-0.5f));
    x = 0.03f;
    y = 0.0f;
    sdl::apply_deadzone(x, y, 0.0f); // no dead zone: unchanged
    CHECK(x == Approx(0.03f));
    CHECK(sdl::stick_byte_of(0.0f) == 0x80);
}

TEST_CASE("custom bindings: several sources, axes as buttons, unknown names reported", "[display_sdl]") {
    input::Bindings keys = input::default_profile().keyboard;
    keys[input::Control::cross] = {"Space", "K"};
    keys[input::Control::circle] = {};
    keys[input::Control::start] = {"NoSuchKey"};
    std::vector<std::string> unknown;
    const sdl::DeviceMap map = sdl::keyboard_map(keys, &unknown);
    CHECK(unknown == std::vector<std::string>{"NoSuchKey"});
    PadState state;
    sdl::apply_map(state, map,
                   [](const sdl::Source& s) { return s.code == SDL_SCANCODE_SPACE ? 1.0f : 0.0f; });
    CHECK(state.buttons == pad_cross);

    input::Bindings pad = input::default_profile().gamepad;
    pad[input::Control::cross] = {"righty+"};   // a stick half as a button: past the middle
    pad[input::Control::left_stick_up] = {"y"}; // a button as a stick direction: full deflection
    pad[input::Control::right_stick_down] = {"bogus+", "rightx+"};
    unknown.clear();
    const sdl::DeviceMap pad_map = sdl::gamepad_map(pad, &unknown);
    CHECK(unknown == std::vector<std::string>{"bogus+"});
    REQUIRE(pad_map[input::Control::right_stick_down].size() == 1);
    CHECK(pad_map[input::Control::right_stick_down][0] ==
          sdl::Source{sdl::Source::Kind::axis, SDL_GAMEPAD_AXIS_RIGHTX, 1});
    sdl::GamepadReading reading;
    reading.axes[SDL_GAMEPAD_AXIS_RIGHTY] = 0.6f;
    reading.buttons[SDL_GAMEPAD_BUTTON_NORTH] = true;
    PadState p;
    sdl::apply_map(p, pad_map, [&reading](const sdl::Source& s) { return reading.amount(s); });
    CHECK((p.buttons & pad_cross) != 0);
    CHECK(p.ly == 0x00);
    reading.axes[SDL_GAMEPAD_AXIS_RIGHTY] = 0.4f;
    PadState q;
    sdl::apply_map(q, pad_map, [&reading](const sdl::Source& s) { return reading.amount(s); });
    CHECK((q.buttons & pad_cross) == 0);
}

TEST_CASE("port assignment: a replugged pad gets its port back", "[display_sdl]") {
    sdl::PortAssigner ports;
    CHECK(ports.connect(10, "guid-a", "") == 0u);
    CHECK(ports.connect(11, "guid-b", "") == 1u);
    CHECK(ports.connect(12, "guid-c", "") == std::nullopt); // both taken: waits
    CHECK(ports.disconnect(10) == 0u);
    CHECK(ports.disconnect(11) == 1u);
    // B comes back first and still takes port 1; A takes port 0.
    CHECK(ports.connect(21, "guid-b", "") == 1u);
    CHECK(ports.connect(20, "guid-a", "") == 0u);
    CHECK(ports.instance_at(0) == 20u);
    CHECK(ports.port_of(21) == 1u);
    CHECK(ports.disconnect(99) == std::nullopt);

    // A new pad prefers a port nobody held; with none left, any free port.
    sdl::PortAssigner fresh;
    CHECK(fresh.connect(1, "guid-a", "") == 0u);
    CHECK(fresh.disconnect(1) == 0u);
    CHECK(fresh.connect(2, "guid-b", "") == 1u); // port 0 waits for A
    CHECK(fresh.connect(3, "guid-c", "") == 0u); // nothing else is free

    // Two pads of one model: told apart by serial when SDL has one.
    sdl::PortAssigner twins;
    CHECK(twins.connect(1, "guid-x", "s1") == 0u);
    CHECK(twins.connect(2, "guid-x", "s2") == 1u);
    twins.disconnect(1);
    twins.disconnect(2);
    CHECK(twins.connect(3, "guid-x", "s2") == 1u);
    CHECK(twins.connect(4, "guid-x", "s1") == 0u);

    // A pinned GUID takes its port, and other pads keep off it.
    sdl::PortAssigner pinned;
    pinned.pin("guid-p", 0);
    CHECK(pinned.connect(1, "guid-q", "") == 1u);
    CHECK(pinned.connect(2, "guid-p", "") == 0u);
}

TEST_CASE("the SDL platform with virtual gamepads: ports survive a replug", "[display_sdl]") {
    auto platform = dummy_platform();
    platform->pump_events();
    if (host_has_gamepads()) {
        SKIP("a gamepad is connected to this host, so the ports are not the virtual pads'");
    }
    const SDL_JoystickID a = attach_pad(0x0001);
    const SDL_JoystickID b = attach_pad(0x0002);
    if (a == 0 || b == 0) {
        SKIP("no virtual joystick: " << SDL_GetError());
    }
    platform->pump_events();
    CHECK(sdl::gamepad_port(*platform, a) == 0u);
    CHECK(sdl::gamepad_port(*platform, b) == 1u);
    CHECK(platform->pad(1).connected);

    // Unplug both; plug B in first: it takes port 1 again, and port 0 stays the keyboard's until A returns.
    SDL_DetachVirtualJoystick(a);
    SDL_DetachVirtualJoystick(b);
    platform->pump_events();
    CHECK_FALSE(platform->pad(1).connected);
    CHECK(platform->pad(0).connected);
    const SDL_JoystickID b2 = attach_pad(0x0002);
    platform->pump_events();
    CHECK(sdl::gamepad_port(*platform, b2) == 1u);
    const SDL_JoystickID a2 = attach_pad(0x0001);
    platform->pump_events();
    CHECK(sdl::gamepad_port(*platform, a2) == 0u);

    // The pad on port 1 reaches port 1's state.
    SDL_Joystick* joystick = SDL_OpenJoystick(b2);
    REQUIRE(joystick != nullptr);
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
    platform->pump_events();
    CHECK(platform->pad(1).buttons == pad_cross);
    CHECK(platform->pad(0).buttons == 0);
    SDL_CloseJoystick(joystick);
    SDL_DetachVirtualJoystick(a2);
    SDL_DetachVirtualJoystick(b2);
    platform->pump_events();
}

TEST_CASE("the SDL platform: dead zone and bindings on a virtual gamepad", "[display_sdl]") {
    WindowConfig config;
    config.input.deadzone_percent = 25;
    config.input.profile.gamepad[input::Control::cross] = {"b"}; // swapped with circle
    config.input.profile.gamepad[input::Control::circle] = {"a"};
    auto platform = dummy_platform(config);
    platform->pump_events();
    if (host_has_gamepads()) {
        SKIP("a gamepad is connected to this host");
    }
    const SDL_JoystickID id = attach_pad(0x0003);
    if (id == 0) {
        SKIP("no virtual joystick: " << SDL_GetError());
    }
    platform->pump_events();
    SDL_Joystick* joystick = SDL_OpenJoystick(id);
    REQUIRE(joystick != nullptr);
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, 6000); // 18 %: inside the zone
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_EAST, true);
    platform->pump_events();
    PadState state = platform->pad(0);
    CHECK(state.lx == 0x80);
    CHECK(state.buttons == pad_cross);
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    platform->pump_events();
    CHECK(platform->pad(0).lx == 0xFF);
    SDL_CloseJoystick(joystick);
    SDL_DetachVirtualJoystick(id);
    platform->pump_events();
}

TEST_CASE("the SDL platform: rumble is scaled by the strength", "[display_sdl]") {
    WindowConfig config;
    config.input.rumble_percent = 50;
    auto platform = dummy_platform(config);
    platform->pump_events();
    if (host_has_gamepads()) {
        SKIP("a gamepad is connected to this host");
    }
    struct Seen {
        Uint16 low = 0;
        Uint16 high = 0;
    } seen;
    const SDL_JoystickID id = attach_pad(0x0004, &seen, [](void* user, Uint16 low, Uint16 high) {
        static_cast<Seen*>(user)->low = low;
        static_cast<Seen*>(user)->high = high;
        return true;
    });
    if (id == 0) {
        SKIP("no virtual joystick: " << SDL_GetError());
    }
    platform->pump_events();
    platform->set_rumble(0, 0x8000, 0xFFFF);
    CHECK(seen.low == 0x4000);
    CHECK(seen.high == 0x7FFF);
    platform->set_rumble(0, 0, 0);
    CHECK(seen.low == 0);
    SDL_DetachVirtualJoystick(id);
    platform->pump_events();
}

TEST_CASE("the SDL platform: the nearest filter and integer scaling", "[display_sdl]") {
    // Two columns, black and white, scaled to the window: nearest keeps a hard edge, linear blends it.
    const gs::FrameImage two = frame_of(2, 1, {{0, 0, 0, 255}, {255, 255, 255, 255}});
    {
        WindowConfig config;
        config.filter = ScaleFilter::nearest;
        auto platform = dummy_platform(config);
        const gs::FrameImage shot = capture(*platform, two, 0.0);
        CHECK(pixel(shot, 150, 120)[0] == 0);
        CHECK(pixel(shot, 170, 120)[0] == 255);
    }
    {
        auto platform = dummy_platform();
        const gs::FrameImage shot = capture(*platform, two, 0.0);
        const std::uint8_t mid = pixel(shot, 150, 120)[0];
        CHECK(mid > 0);
        CHECK(mid < 255);
    }
    {
        // 100x100 at aspect 1 in 320x240 with integer scaling: 200x200 at (60, 20), black around it.
        WindowConfig config;
        config.integer_scaling = true;
        auto platform = dummy_platform(config);
        const gs::FrameImage shot = capture(*platform, frame_of(100, 100, {{255, 0, 0, 255}}), 1.0);
        CHECK(pixel(shot, 60, 20)[0] == 255);
        CHECK(pixel(shot, 259, 219)[0] == 255);
        CHECK(pixel(shot, 59, 120)[0] == 0);
        CHECK(pixel(shot, 160, 220)[0] == 0);
    }
}

TEST_CASE("the SDL platform: window modes and Alt+Enter", "[display_sdl]") {
    auto windowed = dummy_platform();
    CHECK_FALSE(sdl::is_fullscreen(*windowed));
    REQUIRE(sdl::window_of(*windowed) != nullptr);

    // Alt+Enter goes to full screen and back, and its Return never presses Start.
    SDL_Event press{};
    press.type = SDL_EVENT_KEY_DOWN;
    press.key.scancode = SDL_SCANCODE_RETURN;
    press.key.down = true;
    press.key.mod = SDL_KMOD_LALT;
    REQUIRE(SDL_PushEvent(&press));
    windowed->pump_events();
    CHECK(sdl::is_fullscreen(*windowed));
    CHECK((windowed->pad(0).buttons & pad_start) == 0);
    REQUIRE(SDL_PushEvent(&press));
    windowed->pump_events();
    CHECK_FALSE(sdl::is_fullscreen(*windowed));
    // Return alone is still Start.
    press.key.mod = SDL_KMOD_NONE;
    REQUIRE(SDL_PushEvent(&press));
    windowed->pump_events();
    CHECK((windowed->pad(0).buttons & pad_start) != 0);
    windowed.reset();

    // vsync reaches the renderer.
    for (const bool vsync : {false, true}) {
        set_env("SDL_VIDEO_DRIVER", "dummy");
        WindowConfig config;
        config.width = 320;
        config.height = 240;
        config.vsync = vsync;
        auto platform = create_sdl_platform(config);
        int interval = -1;
        REQUIRE(SDL_GetRenderVSync(SDL_GetRenderer(sdl::window_of(*platform)), &interval));
        CHECK(interval == (vsync ? 1 : 0));
    }

    for (const WindowMode mode : {WindowMode::borderless, WindowMode::fullscreen}) {
        WindowConfig config;
        config.mode = mode;
        auto platform = dummy_platform(config);
        CHECK(sdl::is_fullscreen(*platform));
        CHECK((SDL_GetWindowFlags(sdl::window_of(*platform)) & SDL_WINDOW_FULLSCREEN) != 0);
    }
}

TEST_CASE("the SDL audio sink: device by name and volume", "[display_sdl]") {
    set_env("SDL_AUDIO_DRIVER", "dummy");
    const std::vector<std::string> devices = sdl_audio_playback_devices();
    REQUIRE_FALSE(devices.empty()); // the dummy driver offers one

    AudioRing ring(4096);
    SdlAudioConfig config;
    config.latency_ms = 20;
    config.device = "no device is called this";
    config.volume = 30;
    auto sink = make_sdl_audio_sink(config);
    sink->start(ring);
    CHECK_FALSE(sink->device_found()); // the default device instead, with a warning
    sink->stop();

    config.device = devices.front();
    auto named = make_sdl_audio_sink(config);
    named->start(ring);
    CHECK(named->device_found());
    CHECK(named->device_name() == devices.front());
    named->stop();
}
