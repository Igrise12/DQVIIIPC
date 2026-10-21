// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// IPlatform over SDL3 (ADR-0005): one resizable window with an SDL renderer, keyboard and gamepads mapped to
// PadState by the fixed M3 table of sdl_mapping.h, and present() as a texture upload drawn with a 4:3 or 16:9
// fit and bilinear filtering (the scaling PCSX2's presentation used in S5b). Main thread only. Key state is
// tracked from events rather than SDL's keyboard snapshot, so injected events (tests, automation) behave like
// real ones.

#include "runtime/platform/sdl/sdl_platform.h"

#include "runtime/platform/platform.h"
#include "runtime/platform/sdl/sdl_mapping.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dq8::rt {

namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("SDL3: " + what + ": " + SDL_GetError());
}

class SdlPlatform final : public IPlatform {
  public:
    explicit SdlPlatform(const WindowConfig& config) {
        try {
            open(config);
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
        return open_;
    }

    PadState pad(unsigned port) const override {
        PadState state;
        SDL_Gamepad* gamepad = port < gamepads_.size() ? gamepads_[port] : nullptr;
        if (port == 0) {
            sdl::apply_keyboard(state,
                                [this](SDL_Scancode code) { return keys_[static_cast<std::size_t>(code)]; });
        } else if (gamepad == nullptr) {
            state.connected = false;
            return state;
        }
        if (gamepad != nullptr) {
            apply_gamepad(state, gamepad);
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
            const sdl::FitRect fit = sdl::aspect_fit(out_w, out_h, aspect);
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

  private:
    void open(const WindowConfig& config) {
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

        window_ = SDL_CreateWindow(config.title.c_str(), std::max(config.width, 1),
                                   std::max(config.height, 1), SDL_WINDOW_RESIZABLE);
        if (window_ == nullptr) {
            fail("cannot create the window");
        }
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (renderer_ == nullptr) {
            fail("cannot create a renderer");
        }
        if (config.vsync && !SDL_SetRenderVSync(renderer_, 1)) {
            std::fprintf(stderr, "SDL3: vsync unavailable: %s\n", SDL_GetError());
        }
    }

    void release() noexcept {
        for (SDL_Gamepad* gamepad : gamepads_) {
            SDL_CloseGamepad(gamepad);
        }
        gamepads_.clear();
        if (texture_ != nullptr) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }
        if (renderer_ != nullptr) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
        }
        if (window_ != nullptr) {
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
            SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
            texture_w_ = w;
            texture_h_ = h;
        }
        SDL_UpdateTexture(texture_, nullptr, frame.rgba.data(), w * 4);
    }

    void add_gamepad(SDL_JoystickID id) {
        if (!gamepad_) {
            return;
        }
        SDL_Gamepad* gamepad = SDL_OpenGamepad(id);
        if (gamepad == nullptr) {
            std::fprintf(stderr, "SDL3: cannot open gamepad %u: %s\n", static_cast<unsigned>(id),
                         SDL_GetError());
            return;
        }
        gamepads_.push_back(gamepad);
    }

    void remove_gamepad(SDL_JoystickID id) {
        const auto it = std::find_if(gamepads_.begin(), gamepads_.end(),
                                     [id](SDL_Gamepad* gamepad) { return SDL_GetGamepadID(gamepad) == id; });
        if (it != gamepads_.end()) {
            SDL_CloseGamepad(*it);
            gamepads_.erase(it);
        }
    }

    static void apply_gamepad(PadState& state, SDL_Gamepad* gamepad) {
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            const auto button = static_cast<SDL_GamepadButton>(b);
            if (SDL_GetGamepadButton(gamepad, button)) {
                state.buttons =
                    static_cast<std::uint16_t>(state.buttons | sdl::button_for_gamepad_button(button));
            }
        }
        if (sdl::trigger_pressed(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER))) {
            state.buttons = static_cast<std::uint16_t>(state.buttons | pad_l2);
        }
        if (sdl::trigger_pressed(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))) {
            state.buttons = static_cast<std::uint16_t>(state.buttons | pad_r2);
        }
        // A stick off centre overrides the keyboard's stick keys; a centred one leaves them alone.
        const auto stick = [gamepad](SDL_GamepadAxis axis, std::uint8_t& value) {
            const std::uint8_t v = sdl::stick_byte(SDL_GetGamepadAxis(gamepad, axis));
            if (v < 0x70 || v > 0x90) {
                value = v;
            }
        };
        stick(SDL_GAMEPAD_AXIS_LEFTX, state.lx);
        stick(SDL_GAMEPAD_AXIS_LEFTY, state.ly);
        stick(SDL_GAMEPAD_AXIS_RIGHTX, state.rx);
        stick(SDL_GAMEPAD_AXIS_RIGHTY, state.ry);
    }

    bool video_ = false;
    bool gamepad_ = false;
    bool open_ = true;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int texture_w_ = 0;
    int texture_h_ = 0;
    std::array<bool, SDL_SCANCODE_COUNT> keys_{};
    std::vector<SDL_Gamepad*> gamepads_; // in the order they were attached: index = pad port
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

} // namespace dq8::rt
