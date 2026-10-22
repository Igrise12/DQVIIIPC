// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// dq8-launcher (architecture.md §11, ADR-0010): the window, Dear ImGui over SDL3, DPI scaling and the frame
// loop. What the pages show and decide lives in ui/ and model/.
#include "dq8/core/version.h"
#include "model/environment.h"
#include "ui/app.h"
#include "ui/widgets.h"

#include <SDL3/SDL.h>
#include <charconv>
#include <cstdio>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace dq8::launcher {
extern const unsigned char launcher_font[]; // data/fonts/NotoSans-Regular.ttf (embed_file.cmake)
extern const std::size_t launcher_font_size;
} // namespace dq8::launcher

namespace {

namespace fs = std::filesystem;
using namespace dq8::launcher;

constexpr int exit_ok = 0;
constexpr int exit_usage = 2;
constexpr int exit_platform = 3;

struct Args {
    long frames = -1;
    ui::AppOptions app;
    fs::path game;
    fs::path ctl;
    fs::path screenshot;
};

void usage(std::FILE* out) {
    std::fputs(
        "usage: dq8-launcher [options]\n"
        "  --page NAME        open on a page: play, setup, settings, controllers, diagnostics, about\n"
        "  --game FILE        the dq8-game to start (default: next to the launcher)\n"
        "  --dq8ctl FILE      the dq8ctl for the diagnostics export (default: next to the launcher)\n"
        "  --preview          every page over stand-ins: no disc, no toolchain, no game; nothing is written\n"
        "  --frames N         close after N frames (smoke tests; SDL_VIDEO_DRIVER=dummy needs no display)\n"
        "  --screenshot FILE  with --frames: save the last frame as BMP\n"
        "  --version | --help\n",
        out);
}

// The UI's scale: the display's content scale, without the pixel density the renderer already covers.
float ui_scale(SDL_Window* window) {
    const float density = SDL_GetWindowPixelDensity(window);
    float scale = SDL_GetWindowDisplayScale(window);
    if (density > 0.0f) {
        scale /= density;
    }
    return scale > 0.25f ? scale : 1.0f;
}

void save_screenshot(SDL_Renderer* renderer, const fs::path& file) {
    SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr);
    if (shot == nullptr) {
        std::fprintf(stderr, "dq8-launcher: --screenshot: %s\n", SDL_GetError());
        return;
    }
    if (!SDL_SaveBMP(shot, to_utf8(file).c_str())) {
        std::fprintf(stderr, "dq8-launcher: --screenshot: %s\n", SDL_GetError());
    }
    SDL_DestroySurface(shot);
}

int run(const Args& args, const char* argv0) {
    SDL_SetAppMetadata("DQ8-Native", dq8::core::version(), "io.github.dq8native.launcher");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD) && !SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "dq8-launcher: SDL_Init failed: %s\n", SDL_GetError());
        return exit_platform;
    }
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0.25f) {
        scale = 1.0f;
    }
    SDL_Window* window =
        SDL_CreateWindow("DQ8-Native", static_cast<int>(1120.0f * scale), static_cast<int>(760.0f * scale),
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = window != nullptr ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (renderer == nullptr) {
        std::fprintf(stderr, "dq8-launcher: no window: %s\n", SDL_GetError());
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
        return exit_platform;
    }
    SDL_SetWindowMinimumSize(window, 640, 420);
    SDL_SetRenderVSync(renderer, 1);
    scale = ui_scale(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // the launcher keeps its own state (launcher.json), not imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImFontConfig font;
    font.FontDataOwnedByAtlas = false; // static data, never freed
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(launcher_font),
                                   static_cast<int>(launcher_font_size), 0.0f, &font);
    ui::apply_style(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    const char* base = SDL_GetBasePath(); // with a trailing separator
    fs::path exe_dir = base != nullptr ? from_utf8(base) : fs::path();
    if (!exe_dir.empty() && !exe_dir.has_filename()) {
        exe_dir = exe_dir.parent_path();
    }
    ui::AppOptions options = args.app;
    options.self = exe_dir / fs::path(argv0).filename();
    options.demo = options.preview && args.frames >= 0;
    options.remember = args.frames < 0;
    {
        ui::App app(window, renderer, discover_environment(exe_dir, args.game, args.ctl), options);
        // Frames to draw before waiting for events again (Dear ImGui settles over a few frames).
        int settle = 0;
        for (long frame = 0; !app.should_quit() && (args.frames < 0 || frame < args.frames); ++frame) {
            SDL_Event event;
            bool have = false;
            if (args.frames < 0 && settle <= 0) {
                // Idle: sleep until something happens; while work runs, wake often enough to show progress.
                have = SDL_WaitEventTimeout(&event, app.busy() ? 50 : 500);
            } else {
                have = SDL_PollEvent(&event);
            }
            while (have) {
                settle = 3;
                if (!app.handle_event(event)) {
                    ImGui_ImplSDL3_ProcessEvent(&event);
                }
                if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                                                     event.window.windowID == SDL_GetWindowID(window))) {
                    app.request_quit();
                }
                if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                    scale = ui_scale(window);
                    ui::apply_style(scale);
                }
                have = SDL_PollEvent(&event);
            }
            if (app.busy()) {
                settle = 1;
            }
            --settle;
            if (args.frames < 0 && (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0) {
                SDL_Delay(50); // the workers' messages wait in their mailboxes until the window is back
                continue;
            }
            app.update_navigation(io);
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColorFloat(renderer, 0.08f, 0.08f, 0.10f, 1.0f);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            if (!args.screenshot.empty() && args.frames >= 0 && frame + 1 == args.frames) {
                save_screenshot(renderer, args.screenshot);
            }
            SDL_RenderPresent(renderer);
        }
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_ok;
}

bool take_value(int argc, char** argv, int& i, std::string_view name, std::string& out) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "dq8-launcher: %s needs a value\n", std::string(name).c_str());
        return false;
    }
    out = argv[++i];
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        std::string value;
        if (arg == "--help" || arg == "-h") {
            usage(stdout);
            return exit_ok;
        }
        if (arg == "--version") {
            std::printf("dq8-launcher %s\n", dq8::core::version());
            return exit_ok;
        }
        if (arg == "--preview") {
            args.app.preview = true;
            continue;
        }
        if (arg == "--frames") {
            if (!take_value(argc, argv, i, arg, value)) {
                return exit_usage;
            }
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), args.frames);
            if (ec != std::errc() || end != value.data() + value.size() || args.frames < 0) {
                std::fprintf(stderr, "dq8-launcher: --frames needs a count, got '%s'\n", value.c_str());
                return exit_usage;
            }
            continue;
        }
        if (arg == "--page") {
            if (!take_value(argc, argv, i, arg, value)) {
                return exit_usage;
            }
            args.app.start_page = ui::page_from_name(value);
            if (!args.app.start_page) {
                std::fprintf(stderr, "dq8-launcher: unknown page '%s'\n\n", value.c_str());
                usage(stderr);
                return exit_usage;
            }
            continue;
        }
        if (arg == "--game" || arg == "--dq8ctl" || arg == "--screenshot") {
            if (!take_value(argc, argv, i, arg, value)) {
                return exit_usage;
            }
            (arg == "--game" ? args.game : arg == "--dq8ctl" ? args.ctl : args.screenshot) = from_utf8(value);
            continue;
        }
        std::fprintf(stderr, "dq8-launcher: unknown option '%s'\n\n", argv[i]);
        usage(stderr);
        return exit_usage;
    }
    return run(args, argc > 0 ? argv[0] : "dq8-launcher");
}
