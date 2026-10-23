// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// IPlatform over SDL3 (ADR-0005): one window with an SDL renderer, keyboard and gamepads mapped to PadState,
// and present() as a texture upload drawn with an aspect fit. Main thread only. Key state is tracked from
// events rather than SDL's keyboard snapshot, so injected events (tests, automation) behave like real ones.
// Rumble (M4): set_rumble drives SDL_RumbleGamepad on the port's gamepad and renews it while the game keeps a
// motor running.
//
// M5 session C: the window opens windowed, borderless or in exclusive fullscreen (at the desktop's mode) and
// Alt+Enter toggles between the window and full screen (the Return of Alt+Enter never reaches the pad); the
// picture is scaled with a linear or nearest filter, optionally by whole numbers (sdl_mapping.h); bindings
// come from the input profile (sdl_bindings.h), the sticks pass a radial dead zone, gamepads keep their pad
// port across a replug (sdl_ports.h), and rumble is scaled by the configured strength.

#include "runtime/platform/sdl/sdl_platform.h"

#include "runtime/platform/platform.h"
#include "runtime/platform/sdl/sdl_bindings.h"
#include "runtime/platform/sdl/sdl_mapping.h"
#include "runtime/platform/sdl/sdl_ports.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dq8::rt {

namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("SDL3: " + what + ": " + SDL_GetError());
}

std::string guid_string(SDL_GUID guid) {
    std::array<char, 33> text{};
    SDL_GUIDToString(guid, text.data(), static_cast<int>(text.size()));
    return text.data();
}

class SdlPlatform final : public IPlatform {
  public:
    explicit SdlPlatform(const WindowConfig& config) : config_(config) {
        try {
            open();
        } catch (...) {
            release();
            throw;
        }
    }

    ~SdlPlatform() override { release(); }

    SdlPlatform(const SdlPlatform&) = delete;
    SdlPlatform& operator=(const SdlPlatform&) = delete;

    bool pump_events() override {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                open_ = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT) != 0) {
                    if (!event.key.repeat) {
                        toggle_fullscreen();
                    }
                    break; // Alt+Enter is the window's, not Start
                }
                [[fallthrough]];
            case SDL_EVENT_KEY_UP:
                if (event.key.scancode < SDL_SCANCODE_COUNT) {
                    keys_[static_cast<std::size_t>(event.key.scancode)] = event.key.down;
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                keys_.fill(false); // no key stays down while another window has the keyboard
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                add_gamepad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                remove_gamepad(event.gdevice.which);
                break;
            default:
                break;
            }
        }
        // SDL ends a rumble after its duration: a motor the game keeps running is renewed.
        for (unsigned port = 0; port < rumble_.size(); ++port) {
            const Rumble& r = rumble_[port];
            if ((r.low != 0 || r.high != 0) && SDL_GetTicks() - r.sent >= rumble_renew_ms) {
                send_rumble(port);
            }
        }
        return open_;
    }

    void set_rumble(unsigned port, std::uint16_t low, std::uint16_t high) override {
        if (port >= rumble_.size()) {
            return;
        }
        rumble_[port].low = low;
        rumble_[port].high = high;
        send_rumble(port);
    }

    PadState pad(unsigned port) const override {
        PadState state;
        const Pad* gamepad = pad_at(port);
        if (port == 0) {
            sdl::apply_map(state, keyboard_, [this](const sdl::Source& s) {
                return s.kind == sdl::Source::Kind::key && s.code >= 0 && s.code < SDL_SCANCODE_COUNT &&
                               keys_[static_cast<std::size_t>(s.code)]
                           ? 1.0f
                           : 0.0f;
            });
        } else if (gamepad == nullptr) {
            state.connected = false;
            return state;
        }
        if (gamepad != nullptr) {
            const sdl::GamepadReading reading = read(gamepad->handle);
            sdl::apply_map(state, gamepad->map,
                           [&reading](const sdl::Source& s) { return reading.amount(s); });
        }
        sdl::fill_pressure(state);
        return state;
    }

    void present(const gs::FrameImage& frame, double aspect) override {
        if (frame.width != 0 && frame.height != 0 &&
            frame.rgba.size() >= std::size_t{frame.width} * frame.height * 4) {
            upload(frame);
        }
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        if (texture_ != nullptr) {
            int out_w = 0;
            int out_h = 0;
            SDL_GetCurrentRenderOutputSize(renderer_, &out_w, &out_h);
            const sdl::FitRect fit =
                sdl::present_rect(out_w, out_h, static_cast<std::uint32_t>(texture_w_),
                                  static_cast<std::uint32_t>(texture_h_), aspect, config_.integer_scaling);
            const SDL_FRect dst{fit.x, fit.y, fit.w, fit.h};
            SDL_RenderTexture(renderer_, texture_, nullptr, &dst);
        }
        if (capture_requested_) {
            capture_requested_ = false;
            capture_ready_ = read_back(capture_);
        }
        SDL_RenderPresent(renderer_);
    }

    void request_capture() noexcept {
        capture_requested_ = true;
        capture_ready_ = false;
    }

    bool take_capture(gs::FrameImage& out) {
        if (!capture_ready_) {
            return false;
        }
        out = std::move(capture_);
        capture_ = {};
        capture_ready_ = false;
        return true;
    }

    SDL_Window* window() const noexcept { return window_; }
    bool fullscreen() const noexcept { return fullscreen_; }
    std::optional<unsigned> port_of(SDL_JoystickID id) const { return ports_.port_of(id); }

  private:
    struct Pad {
        SDL_Gamepad* handle = nullptr;
        sdl::DeviceMap map;
    };

    void open() {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            fail("cannot initialise video");
        }
        video_ = true;
        // Gamepads are optional: without them (or without udev) there is still a window and the keyboard.
        if (SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            gamepad_ = true;
        } else {
            std::fprintf(stderr, "SDL3: gamepads unavailable: %s\n", SDL_GetError());
        }

        std::vector<std::string> unknown;
        keyboard_ = sdl::keyboard_map(config_.input.profile.keyboard, &unknown);
        warn_unknown(unknown, "keyboard");
        for (const core::input::DeviceProfile& d : config_.input.profile.devices) {
            if (d.port) {
                ports_.pin(d.guid, *d.port);
            }
        }

        window_ =
            SDL_CreateWindow(config_.title.c_str(), std::max(config_.width, 1), std::max(config_.height, 1),
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (window_ == nullptr) {
            fail("cannot create the window");
        }
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (renderer_ == nullptr) {
            fail("cannot create a renderer");
        }
        if (!SDL_SetRenderVSync(renderer_, config_.vsync ? 1 : 0) && config_.vsync) {
            std::fprintf(stderr, "SDL3: vsync unavailable: %s\n", SDL_GetError());
        }
        if (config_.mode != WindowMode::windowed) {
            enter_fullscreen(config_.mode);
        }
    }

    void release() noexcept {
        for (auto& [id, pad] : pads_) {
            SDL_CloseGamepad(pad.handle);
        }
        pads_.clear();
        if (texture_ != nullptr) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }
        if (renderer_ != nullptr) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
        }
        if (window_ != nullptr) {
            if (fullscreen_) {
                SDL_ShowCursor();
            }
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }
        if (gamepad_) {
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
            gamepad_ = false;
        }
        if (video_) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            video_ = false;
        }
        // SDL's subsystems are reference-counted; tear SDL down once nothing else in the process uses it.
        if (SDL_WasInit(0) == 0) {
            SDL_Quit();
        }
    }

    // Borderless covers the display at the desktop's mode; exclusive switches to the closest fullscreen mode
    // to the desktop's (the same size on most systems, but the window owns the display).
    void enter_fullscreen(WindowMode mode) {
        const SDL_DisplayMode* use = nullptr;
        SDL_DisplayMode closest{};
        if (mode == WindowMode::fullscreen) {
            const SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
            const SDL_DisplayMode* desktop = display != 0 ? SDL_GetDesktopDisplayMode(display) : nullptr;
            if (desktop != nullptr &&
                SDL_GetClosestFullscreenDisplayMode(display, desktop->w, desktop->h, desktop->refresh_rate,
                                                    true, &closest)) {
                use = &closest;
            } else {
                std::fprintf(stderr, "SDL3: no exclusive fullscreen mode (%s); using borderless fullscreen\n",
                             SDL_GetError());
            }
        }
        if (!SDL_SetWindowFullscreenMode(window_, use) || !SDL_SetWindowFullscreen(window_, true)) {
            std::fprintf(stderr, "SDL3: cannot enter fullscreen: %s\n", SDL_GetError());
            return;
        }
        fullscreen_ = true;
        SDL_HideCursor();
    }

    void leave_fullscreen() {
        if (!SDL_SetWindowFullscreen(window_, false)) {
            std::fprintf(stderr, "SDL3: cannot leave fullscreen: %s\n", SDL_GetError());
            return;
        }
        fullscreen_ = false;
        SDL_ShowCursor();
    }

    // Alt+Enter: the window and the configured full screen (borderless when the window mode is windowed).
    void toggle_fullscreen() {
        if (fullscreen_) {
            leave_fullscreen();
        } else {
            enter_fullscreen(config_.mode == WindowMode::windowed ? WindowMode::borderless : config_.mode);
        }
    }

    // The render target as drawn so far (before SDL_RenderPresent), as RGBA8.
    bool read_back(gs::FrameImage& out) {
        SDL_Surface* raw = SDL_RenderReadPixels(renderer_, nullptr);
        if (raw == nullptr) {
            std::fprintf(stderr, "SDL3: cannot read the window back: %s\n", SDL_GetError());
            return false;
        }
        SDL_Surface* rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        if (rgba == nullptr) {
            std::fprintf(stderr, "SDL3: cannot convert the window copy: %s\n", SDL_GetError());
            return false;
        }
        const auto w = static_cast<std::size_t>(rgba->w);
        const auto h = static_cast<std::size_t>(rgba->h);
        out.width = static_cast<std::uint32_t>(w);
        out.height = static_cast<std::uint32_t>(h);
        out.rgba.resize(w * h * 4);
        const auto* src = static_cast<const std::uint8_t*>(rgba->pixels);
        for (std::size_t y = 0; y < h; ++y) {
            std::memcpy(out.rgba.data() + y * w * 4, src + y * static_cast<std::size_t>(rgba->pitch), w * 4);
        }
        SDL_DestroySurface(rgba);
        return true;
    }

    void upload(const gs::FrameImage& frame) {
        const int w = static_cast<int>(frame.width);
        const int h = static_cast<int>(frame.height);
        if (texture_ == nullptr || texture_w_ != w || texture_h_ != h) {
            if (texture_ != nullptr) {
                SDL_DestroyTexture(texture_);
            }
            // RGBA32 is the byte order R, G, B, A in memory on every host, which is FrameImage's layout.
            texture_ =
                SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
            if (texture_ == nullptr) {
                fail("cannot create a " + std::to_string(w) + "x" + std::to_string(h) + " texture");
            }
            SDL_SetTextureScaleMode(texture_, config_.filter == ScaleFilter::nearest ? SDL_SCALEMODE_NEAREST
                                                                                     : SDL_SCALEMODE_LINEAR);
            texture_w_ = w;
            texture_h_ = h;
        }
        SDL_UpdateTexture(texture_, nullptr, frame.rgba.data(), w * 4);
    }

    void add_gamepad(SDL_JoystickID id) {
        if (!gamepad_ || pads_.count(id) != 0) {
            return;
        }
        SDL_Gamepad* handle = SDL_OpenGamepad(id);
        if (handle == nullptr) {
            std::fprintf(stderr, "SDL3: cannot open gamepad %u: %s\n", static_cast<unsigned>(id),
                         SDL_GetError());
            return;
        }
        const std::string guid = guid_string(SDL_GetGamepadGUIDForID(id));
        const char* serial = SDL_GetGamepadSerial(handle);
        const char* name = SDL_GetGamepadName(handle);
        Pad pad;
        pad.handle = handle;
        std::vector<std::string> unknown;
        pad.map = sdl::gamepad_map(config_.input.profile.gamepad_for(guid), &unknown);
        warn_unknown(unknown, "gamepad");
        pads_.emplace(id, std::move(pad));
        const auto port = ports_.connect(id, guid, serial != nullptr ? serial : "");
        if (port) {
            std::fprintf(stderr, "input: %s (%s) on pad port %u\n", name != nullptr ? name : "gamepad",
                         guid.c_str(), *port + 1);
            send_rumble(*port); // a pad plugged in while a motor runs
        } else {
            std::fprintf(stderr, "input: %s (%s) waits: both pad ports are taken\n",
                         name != nullptr ? name : "gamepad", guid.c_str());
        }
    }

    void remove_gamepad(SDL_JoystickID id) {
        const auto it = pads_.find(id);
        if (it == pads_.end()) {
            return;
        }
        if (const auto port = ports_.disconnect(id)) {
            std::fprintf(stderr, "input: the gamepad on pad port %u left; the port waits for it\n",
                         *port + 1);
        }
        SDL_CloseGamepad(it->second.handle);
        pads_.erase(it);
    }

    const Pad* pad_at(unsigned port) const {
        const auto id = ports_.instance_at(port);
        if (!id) {
            return nullptr;
        }
        const auto it = pads_.find(*id);
        return it != pads_.end() ? &it->second : nullptr;
    }

    // The gamepad's controls now, its sticks through the dead zone.
    sdl::GamepadReading read(SDL_Gamepad* gamepad) const {
        sdl::GamepadReading r;
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
            r.axes[static_cast<std::size_t>(a)] =
                sdl::axis_amount(SDL_GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(a)));
        }
        const float deadzone =
            static_cast<float>(std::min<std::uint32_t>(config_.input.deadzone_percent, 99)) / 100.0f;
        sdl::apply_deadzone(r.axes[SDL_GAMEPAD_AXIS_LEFTX], r.axes[SDL_GAMEPAD_AXIS_LEFTY], deadzone);
        sdl::apply_deadzone(r.axes[SDL_GAMEPAD_AXIS_RIGHTX], r.axes[SDL_GAMEPAD_AXIS_RIGHTY], deadzone);
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            r.buttons[static_cast<std::size_t>(b)] =
                SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(b));
        }
        return r;
    }

    // Drives the motors of the gamepad at `port`, if there is one, for twice the renewal period, scaled by
    // the configured strength.
    void send_rumble(unsigned port) {
        const Pad* pad = pad_at(port);
        if (port >= rumble_.size() || pad == nullptr) {
            return;
        }
        Rumble& r = rumble_[port];
        const std::uint32_t strength = std::min<std::uint32_t>(config_.input.rumble_percent, 100);
        const auto scaled = [strength](std::uint16_t v) {
            return static_cast<std::uint16_t>(v * strength / 100);
        };
        const bool on = r.low != 0 || r.high != 0;
        if (!SDL_RumbleGamepad(pad->handle, scaled(r.low), scaled(r.high), on ? 2 * rumble_renew_ms : 0) &&
            on && !rumble_warned_) {
            rumble_warned_ = true;
            std::fprintf(stderr, "SDL3: the gamepad does not rumble: %s\n", SDL_GetError());
        }
        r.sent = SDL_GetTicks();
    }

    void warn_unknown(const std::vector<std::string>& names, const char* device) {
        for (const std::string& name : names) {
            if (warned_names_.insert(std::string(device) + ":" + name).second) {
                std::fprintf(stderr, "input: the %s binding '%s' is not a name SDL knows; it is ignored\n",
                             device, name.c_str());
            }
        }
    }

    WindowConfig config_;
    bool video_ = false;
    bool gamepad_ = false;
    bool open_ = true;
    bool fullscreen_ = false;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int texture_w_ = 0;
    int texture_h_ = 0;
    std::array<bool, SDL_SCANCODE_COUNT> keys_{};
    sdl::DeviceMap keyboard_;
    std::map<SDL_JoystickID, Pad> pads_; // every open gamepad, with a port or waiting for one
    sdl::PortAssigner ports_;
    std::set<std::string> warned_names_;
    struct Rumble {
        std::uint16_t low = 0;
        std::uint16_t high = 0;
        std::uint64_t sent = 0; // SDL_GetTicks() of the last request
    };
    static constexpr std::uint32_t rumble_renew_ms = 1000;
    std::array<Rumble, 2> rumble_{};
    bool rumble_warned_ = false;
    bool capture_requested_ = false;
    bool capture_ready_ = false;
    gs::FrameImage capture_;
};

} // namespace

std::unique_ptr<IPlatform> create_sdl_platform(const WindowConfig& config) {
    return std::make_unique<SdlPlatform>(config);
}

bool sdl::request_window_capture(IPlatform& platform) {
    auto* sdl_platform = dynamic_cast<SdlPlatform*>(&platform);
    if (sdl_platform == nullptr) {
        return false;
    }
    sdl_platform->request_capture();
    return true;
}

bool sdl::take_window_capture(IPlatform& platform, gs::FrameImage& out) {
    auto* sdl_platform = dynamic_cast<SdlPlatform*>(&platform);
    return sdl_platform != nullptr && sdl_platform->take_capture(out);
}

SDL_Window* sdl::window_of(IPlatform& platform) {
    auto* sdl_platform = dynamic_cast<SdlPlatform*>(&platform);
    return sdl_platform != nullptr ? sdl_platform->window() : nullptr;
}

bool sdl::is_fullscreen(IPlatform& platform) {
    auto* sdl_platform = dynamic_cast<SdlPlatform*>(&platform);
    return sdl_platform != nullptr && sdl_platform->fullscreen();
}

std::optional<unsigned> sdl::gamepad_port(IPlatform& platform, std::uint32_t joystick_id) {
    auto* sdl_platform = dynamic_cast<SdlPlatform*>(&platform);
    return sdl_platform != nullptr ? sdl_platform->port_of(joystick_id) : std::nullopt;
}

} // namespace dq8::rt
