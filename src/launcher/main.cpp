// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// dq8-launcher, M5 day 0: a placeholder window that proves the Dear ImGui over SDL3 stack builds and runs
// (ADR-0010). M5 session B replaces it with the launcher of Docs/milestones/M5-launcher.md.
#include "dq8/core/version.h"

#include <SDL3/SDL.h>
#include <charconv>
#include <cstdio>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <string>
#include <string_view>
#include <system_error>

namespace {

constexpr int exit_ok = 0;
constexpr int exit_usage = 2;
constexpr int exit_platform = 3;

void usage(std::FILE* out) {
    std::fputs("usage: dq8-launcher [--frames N] [--version] [--help]\n"
               "  --frames N   close after N frames (smoke tests; SDL_VIDEO_DRIVER=dummy needs no display)\n",
               out);
}

int run(long frames) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "dq8-launcher: SDL_Init failed: %s\n", SDL_GetError());
        return exit_platform;
    }
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0.0f) {
        scale = 1.0f;
    }
    SDL_Window* window =
        SDL_CreateWindow("DQ8-Native", static_cast<int>(960.0f * scale), static_cast<int>(600.0f * scale),
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
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // the launcher keeps its own state in the settings store, not imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    const std::string version = std::string("dq8-launcher ") + dq8::core::version();
    bool done = false;
    for (long frame = 0; !done && (frames < 0 || frame < frames); ++frame) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                                                 event.window.windowID == SDL_GetWindowID(window))) {
                done = true;
            }
        }
        if (frames < 0 && (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0) {
            SDL_Delay(10);
            continue;
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("DQ8-Native", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted(version.c_str());
        ImGui::Separator();
        ImGui::TextWrapped(
            "The launcher is under construction (M5). Until it is ready, the game is set up with "
            "dq8ctl and started with dq8-game from a terminal.");
        if (ImGui::Button("Close")) {
            done = true;
        }
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColorFloat(renderer, 0.08f, 0.08f, 0.10f, 1.0f);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_ok;
}

} // namespace

int main(int argc, char** argv) {
    long frames = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(stdout);
            return exit_ok;
        }
        if (arg == "--version") {
            std::printf("dq8-launcher %s\n", dq8::core::version());
            return exit_ok;
        }
        if (arg == "--frames" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), frames);
            if (ec != std::errc() || end != value.data() + value.size() || frames < 0) {
                std::fprintf(stderr, "dq8-launcher: --frames needs a count, got '%s'\n", argv[i]);
                return exit_usage;
            }
            continue;
        }
        std::fprintf(stderr, "dq8-launcher: unknown option '%s'\n\n", argv[i]);
        usage(stderr);
        return exit_usage;
    }
    return run(frames);
}
