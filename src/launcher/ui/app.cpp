// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "ui/app.h"

#include "core/platform/files.h"
#include "dq8/core/version.h"
#include "model/core_backends.h"
#include "model/preview.h"
#include "ui/widgets.h"

#include <imgui.h>
#include <system_error>
#include <utility>

namespace dq8::launcher::ui {

namespace fs = std::filesystem;

namespace {

constexpr Page all_pages[] = {Page::play,        Page::setup,       Page::settings,
                              Page::controllers, Page::diagnostics, Page::about};

const char* page_title(Page page) {
    switch (page) {
    case Page::play:
        return "Play";
    case Page::setup:
        return "Setup";
    case Page::settings:
        return "Settings";
    case Page::controllers:
        return "Controllers";
    case Page::diagnostics:
        return "Diagnostics";
    case Page::about:
        return "About";
    }
    return "";
}

} // namespace

struct App::PendingDialog {
    std::weak_ptr<Mailbox<DialogAnswer>> mailbox;
    DialogPurpose purpose;
    std::string setting_key;
};

std::optional<Page> page_from_name(std::string_view name) noexcept {
    for (const Page p : all_pages) {
        if (name == page_name(p)) {
            return p;
        }
    }
    return std::nullopt;
}

const char* page_name(Page page) noexcept {
    switch (page) {
    case Page::play:
        return "play";
    case Page::setup:
        return "setup";
    case Page::settings:
        return "settings";
    case Page::controllers:
        return "controllers";
    case Page::diagnostics:
        return "diagnostics";
    case Page::about:
        return "about";
    }
    return "play";
}

App::App(SDL_Window* window, SDL_Renderer* renderer, Environment env, AppOptions options)
    : window_(window), renderer_(renderer), env_(std::move(env)), options_(options) {
    state_ = load_launcher_state(options_.preview ? fs::path() : env_.state_file);
    const fs::path exe = env_.executable_dir;

    if (options_.preview) {
        wizard_backend_ = preview::wizard_backend();
        // The launcher's own executable stands in for the disc image, so Play is enabled.
        library_ =
            std::make_unique<LibraryModel>(preview::library_backend(options_.self, exe), core::version());
        game_ = std::make_unique<GameSupervisor>(thread_executors(), preview::game_runner());
        diagnostics_ =
            std::make_unique<DiagnosticsModel>(preview::diagnostics_exporter(), thread_executors());
        settings_.emplace(preview::settings_backend(exe));
    } else {
        wizard_backend_ = core_wizard_backend(exe);
        library_ = std::make_unique<LibraryModel>(registry_library(exe), core::version());
        game_ = std::make_unique<GameSupervisor>(thread_executors());
        diagnostics_ = std::make_unique<DiagnosticsModel>(core_diagnostics(exe), thread_executors());
        try {
            settings_.emplace(open_store_settings(exe));
        } catch (...) {
            settings_problem_ = explain_current_exception();
        }
    }
    // setup::Options::executable_dir: where dq8-game (the smoke test) and the tools beside it are.
    wizard_ = std::make_unique<Wizard>(*wizard_backend_, thread_executors(),
                                       env_.game.empty() ? exe : env_.game.parent_path());
    wizard_->set_game_executable(env_.game);
    // Session C's profile files (the ones dq8-game reads); in memory under --preview or without a home.
    if (!options_.preview && !env_.config_dir.empty()) {
        profiles_ = std::make_unique<FileProfileStore>(env_.config_dir);
    } else {
        profiles_ = std::make_unique<MemoryProfileStore>();
    }
    std::string profile = "default";
    if (settings_) {
        try {
            profile = format_value(settings_->backend().value("input.profile"));
        } catch (...) {
        }
    }
    controllers_ = std::make_unique<ControllerModel>(*profiles_, profile);

    if (const auto notices =
            find_shared_file(exe, "THIRD_PARTY_NOTICES.md",
                             fs::path("share") / "doc" / "dq8-native" / "THIRD_PARTY_NOTICES.md")) {
        try {
            notices_ = parse_markdown(core::platform::read_text_file(*notices, 4u << 20));
            notices_file_ = to_utf8(*notices);
        } catch (...) {
        }
    }

    library_->refresh();
    if (options_.start_page) {
        page_ = *options_.start_page;
    } else if (!library_->empty()) {
        page_ = page_from_name(state_.screen).value_or(Page::play);
        if (page_ == Page::setup) {
            page_ = Page::play;
        }
    } else {
        page_ = Page::setup;
    }
}

App::~App() {
    save_state();
}

void App::save_state() {
    if (env_.state_file.empty() || options_.preview || !options_.remember) {
        return;
    }
    state_.screen = page_name(page_);
    try {
        save_launcher_state(env_.state_file, state_);
    } catch (...) {
        // Remembering the page is a convenience; a read-only home must not stop the launcher.
    }
}

bool App::busy() const {
    return wizard_->busy() || game_->active() || diagnostics_->state() == ExportState::running ||
           controllers_->capturing().has_value();
}

bool App::handle_event(const SDL_Event& event) {
    if (gamepads_.on_event(event)) {
        controllers_->set_connected(gamepads_.connected());
    }
    // Key releases always reach Dear ImGui, so it never believes a key is still held.
    if (page_ == Page::controllers && controllers_->capturing() && event.type != SDL_EVENT_KEY_UP) {
        if (const auto signal = to_signal(event, gamepads_)) {
            return controllers_->on_signal(*signal, std::chrono::steady_clock::now());
        }
    }
    return false;
}

void App::update_navigation(ImGuiIO& io) {
    if (controllers_->capturing()) {
        nav_guard_ = true;
    } else if (nav_guard_) {
        bool held = false;
        for (SDL_Gamepad* pad : gamepads_.all()) {
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
                held = held || SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b));
            }
        }
        nav_guard_ = held;
    }
    if (nav_guard_) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    }
}

void App::request_quit() {
    if (wizard_->busy() || game_->active()) {
        confirm_quit_ = true;
        return;
    }
    quit_ = true;
}

void SDLCALL App::dialog_callback(void* userdata, const char* const* files, int /*filter*/) {
    std::unique_ptr<PendingDialog> pending(static_cast<PendingDialog*>(userdata));
    const std::shared_ptr<Mailbox<DialogAnswer>> mailbox = pending->mailbox.lock();
    if (!mailbox) {
        return;
    }
    DialogAnswer answer{pending->purpose, pending->setting_key, std::nullopt};
    if (files != nullptr && files[0] != nullptr) {
        answer.path = files[0];
    }
    mailbox->post(std::move(answer));
}

void App::open_dialog(DialogPurpose purpose, std::string setting_key) {
    auto* pending = new PendingDialog{dialogs_, purpose, std::move(setting_key)};
    // SDL keeps using the filters until the callback runs, so they are static.
    static const SDL_DialogFileFilter image_filters[] = {{"Disc images", "iso"}, {"All files", "*"}};
    static const SDL_DialogFileFilter text_filters[] = {{"Text files", "txt"}, {"All files", "*"}};
    switch (purpose) {
    case DialogPurpose::disc_image: {
        const std::string start =
            state_.last_image.empty() ? std::string() : to_utf8(state_.last_image.parent_path());
        SDL_ShowOpenFileDialog(dialog_callback, pending, window_, image_filters, 2,
                               start.empty() ? nullptr : start.c_str(), false);
        break;
    }
    case DialogPurpose::setting_folder:
        SDL_ShowOpenFolderDialog(dialog_callback, pending, window_, nullptr, false);
        break;
    case DialogPurpose::diagnostics_file: {
        const fs::path dir = env_.data_dir.empty() ? fs::path() : env_.data_dir;
        const std::string start = to_utf8(dir / default_diagnostics_name(std::time(nullptr)));
        SDL_ShowSaveFileDialog(dialog_callback, pending, window_, text_filters, 2, start.c_str());
        break;
    }
    }
}

void App::on_dialog(const DialogAnswer& answer) {
    if (!answer.path) {
        return;
    }
    const fs::path path = from_utf8(*answer.path);
    switch (answer.purpose) {
    case DialogPurpose::disc_image:
        state_.last_image = path;
        save_state();
        wizard_->choose_image(path);
        break;
    case DialogPurpose::setting_folder:
        if (settings_) {
            settings_->set(answer.setting_key, *answer.path);
        }
        break;
    case DialogPurpose::diagnostics_file:
        diagnostics_->start(path);
        break;
    }
}

fs::path App::config_file_for_play() {
    if (!settings_) {
        return {};
    }
    if (settings_->dirty()) {
        settings_->save();
    }
    std::error_code ec;
    const fs::path file = settings_->file();
    return !file.empty() && fs::is_regular_file(file, ec) ? file : fs::path();
}

void App::play() {
    if (controllers_->dirty()) {
        controllers_->save();
    }
    const fs::path game = options_.preview ? options_.self : env_.game; // the preview's runner starts nothing
    if (const auto request =
            library_->play_request(game, options_.preview ? fs::path() : config_file_for_play())) {
        game_->start(*request);
    }
}

void App::frame() {
    if (options_.demo && options_.preview) {
        if (page_ == Page::setup && wizard_->step() == WizardStep::choose_image) {
            wizard_->choose_image(options_.self);
        } else if (page_ == Page::setup && wizard_->step() == WizardStep::ready) {
            wizard_->start_setup();
        } else if (page_ == Page::play && game_->session().phase() == GamePhase::idle) {
            play();
        } else if (page_ == Page::controllers && !controllers_->capturing()) {
            controllers_->begin_capture(0, false, std::chrono::steady_clock::now());
        } else if (page_ == Page::diagnostics && diagnostics_->state() == ExportState::idle) {
            diagnostics_->start(env_.data_dir / "diagnostics" / "preview.txt"); // the preview writes nothing
        }
    }
    for (const DialogAnswer& answer : dialogs_->take()) {
        on_dialog(answer);
    }
    wizard_->pump();
    game_->pump();
    diagnostics_->pump();
    controllers_->tick(std::chrono::steady_clock::now());
    if (wizard_->step() == WizardStep::finished) {
        if (!library_refreshed_after_setup_) {
            library_refreshed_after_setup_ = true;
            library_->refresh();
        }
    } else {
        library_refreshed_after_setup_ = false;
    }
    // Auto-save: once nothing is being dragged or typed into.
    if (!ImGui::IsAnyItemActive()) {
        if (settings_ && settings_->dirty() && !settings_->problem()) {
            settings_->save();
        }
        if (controllers_->dirty() && !controllers_->capturing() && !controllers_->problem()) {
            controllers_->save();
        }
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("DQ8-Native", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    draw_sidebar();
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGui::GetStyle().WindowPadding);
    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    switch (page_) {
    case Page::play:
        draw_play_page(*this);
        break;
    case Page::setup:
        draw_setup_page(*this);
        break;
    case Page::settings:
        draw_settings_page(*this);
        break;
    case Page::controllers:
        draw_controllers_page(*this);
        break;
    case Page::diagnostics:
        draw_diagnostics_page(*this);
        break;
    case Page::about:
        draw_about_page(*this);
        break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    draw_quit_modal();
    ImGui::End();
}

void App::draw_sidebar() {
    const float width = 210.0f * ImGui::GetStyle().FontScaleDpi;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.08f, 0.10f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGui::GetStyle().WindowPadding);
    ImGui::BeginChild("sidebar", ImVec2(width, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    heading("DQ8-Native", 1.35f);
    dimmed(std::string("Version ") + core::version() + (options_.preview ? " (preview)" : ""));
    ImGui::Dummy(ImVec2(0, 8.0f * ImGui::GetStyle().FontScaleDpi));
    for (const Page p : all_pages) {
        std::string label = page_title(p);
        if (p == Page::play && game_->active()) {
            label += "  (running)";
        }
        if (p == Page::setup &&
            (wizard_->step() == WizardStep::running || wizard_->step() == WizardStep::cancelling)) {
            label += "  " + std::to_string(static_cast<int>(wizard_->progress().overall() * 100.0)) + "%";
        }
        label += "##nav";
        if (ImGui::Selectable(label.c_str(), page_ == p, ImGuiSelectableFlags_None,
                              ImVec2(0, ImGui::GetFrameHeight()))) {
            page_ = p;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void App::draw_quit_modal() {
    if (confirm_quit_) {
        ImGui::OpenPopup("Quit the launcher?");
        confirm_quit_ = false;
    }
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480.0f * ImGui::GetStyle().FontScaleDpi, 0));
    if (ImGui::BeginPopupModal("Quit the launcher?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (game_->active()) {
            wrapped(
                "The game is still running. Quitting the launcher stops it; unsaved progress in the game is "
                "lost.");
        } else {
            wrapped("The setup is still running. Quitting cancels it; the next setup continues where it "
                    "stopped.");
        }
        ImGui::Spacing();
        if (ImGui::Button(game_->active() ? "Stop the game and quit" : "Cancel the setup and quit")) {
            game_->stop();
            wizard_->cancel();
            quit_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (primary_button("Keep running") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace dq8::launcher::ui
