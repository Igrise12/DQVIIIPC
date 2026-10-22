// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "model/controllers.h"
#include "model/diagnostics.h"
#include "model/environment.h"
#include "model/game_session.h"
#include "model/launcher_state.h"
#include "model/library.h"
#include "model/markdown.h"
#include "model/settings_model.h"
#include "model/wizard.h"
#include "ui/sdl_input.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The launcher window: a sidebar of pages and the page itself. The App owns the view-models, pumps them once
// a frame and routes SDL events (file dialogs, gamepads, live capture); each page in page_*.cpp only draws.
namespace dq8::launcher::ui {

enum class Page { play, setup, settings, controllers, diagnostics, about };
std::optional<Page> page_from_name(std::string_view name) noexcept;
const char* page_name(Page page) noexcept;

struct AppOptions {
    bool preview = false;           // stand-ins for core, the setup and the game (model/preview.h)
    std::optional<Page> start_page; // --page
    std::filesystem::path self;     // the launcher's executable (the preview's stand-in disc image)
    // With --preview and --frames (the smoke tests): the setup and Play start by themselves, so their running
    // states are drawn too.
    bool demo = false;
    // False with --frames: a smoke test reads the user's state but never writes it.
    bool remember = true;
};

// What a native file dialog was opened for.
enum class DialogPurpose { disc_image, setting_folder, diagnostics_file };

class App {
  public:
    App(SDL_Window* window, SDL_Renderer* renderer, Environment env, AppOptions options);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // True when the event was used (live capture) and must not reach Dear ImGui.
    bool handle_event(const SDL_Event& event);
    // Before each Dear ImGui frame: gamepad navigation is off while a binding is captured and until every pad
    // button is released after it, so the press that binds does not also press the focused button (Dear ImGui
    // reads the pads' state itself, not through the events handle_event() holds back).
    void update_navigation(ImGuiIO& io);
    // The window's close button or the system asked to quit.
    void request_quit();
    bool should_quit() const noexcept { return quit_; }
    // Work is running that needs frames (setup, a game, an export); the loop then waits less for events.
    bool busy() const;

    void frame();

    // For the pages.
    SDL_Window* window() const noexcept { return window_; }
    SDL_Renderer* renderer() const noexcept { return renderer_; }
    const Environment& env() const noexcept { return env_; }
    bool preview() const noexcept { return options_.preview; }
    Page page() const noexcept { return page_; }
    void go(Page page) noexcept { page_ = page; }

    Wizard& wizard() noexcept { return *wizard_; }
    LibraryModel& library() noexcept { return *library_; }
    GameSupervisor& game() noexcept { return *game_; }
    SettingsModel* settings() noexcept { return settings_ ? &*settings_ : nullptr; }
    const std::optional<Problem>& settings_problem() const noexcept { return settings_problem_; }
    ControllerModel& controllers() noexcept { return *controllers_; }
    DiagnosticsModel& diagnostics() noexcept { return *diagnostics_; }
    Gamepads& gamepads() noexcept { return gamepads_; }
    LauncherState& state() noexcept { return state_; }
    const std::vector<MdBlock>& notices() const noexcept { return notices_; }
    const std::string& notices_file() const noexcept { return notices_file_; }

    // Opens a native dialog; the answer arrives in a later frame through on_dialog().
    void open_dialog(DialogPurpose purpose, std::string setting_key = {});
    // Starts the selected build.
    void play();
    // The user's settings file for --config: the store's file when it exists.
    std::filesystem::path config_file_for_play();
    void save_state();

  private:
    struct DialogAnswer {
        DialogPurpose purpose;
        std::string setting_key;
        std::optional<std::string> path; // nullopt: cancelled or failed
    };
    void on_dialog(const DialogAnswer& answer);
    void draw_sidebar();
    void draw_quit_modal();
    static void SDLCALL dialog_callback(void* userdata, const char* const* files, int filter);

    SDL_Window* window_;
    SDL_Renderer* renderer_;
    Environment env_;
    AppOptions options_;
    Page page_ = Page::setup;
    bool quit_ = false;
    bool confirm_quit_ = false;
    bool library_refreshed_after_setup_ = false;
    bool nav_guard_ = false;

    std::unique_ptr<WizardBackend> wizard_backend_;
    std::unique_ptr<Wizard> wizard_;
    std::unique_ptr<LibraryModel> library_;
    std::unique_ptr<GameSupervisor> game_;
    std::optional<SettingsModel> settings_;
    std::optional<Problem> settings_problem_;
    std::unique_ptr<ProfileStore> profiles_;
    std::unique_ptr<ControllerModel> controllers_;
    std::unique_ptr<DiagnosticsModel> diagnostics_;
    Gamepads gamepads_;
    LauncherState state_;
    std::vector<MdBlock> notices_;
    std::string notices_file_;

    struct PendingDialog;
    std::shared_ptr<Mailbox<DialogAnswer>> dialogs_ = std::make_shared<Mailbox<DialogAnswer>>();
};

// The pages (page_*.cpp).
void draw_play_page(App& app);
void draw_setup_page(App& app);
void draw_settings_page(App& app);
void draw_controllers_page(App& app);
void draw_diagnostics_page(App& app);
void draw_about_page(App& app);

} // namespace dq8::launcher::ui
