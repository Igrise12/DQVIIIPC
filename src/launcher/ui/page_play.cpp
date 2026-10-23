// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The Play page: the registered builds, Play and Stop, and what the running or last game reported.
#include "ui/app.h"
#include "ui/widgets.h"

#include <algorithm>
#include <cstdio>
#include <imgui.h>

namespace dq8::launcher::ui {

namespace {

void draw_entry(const LibraryEntry& e) {
    subheading(e.title.empty() ? e.install.profile_id : e.title);
    dimmed(e.install.profile_id + ", built " +
           (e.install.built_at.empty() ? "at an unknown time" : e.install.built_at) +
           (e.install.host_version.empty() ? "" : " by version " + e.install.host_version));
    dimmed("Disc image: " + to_utf8(e.install.iso));
    if (!e.image_found) {
        coloured(colour::bad,
                 "The disc image is no longer there. Set the game up again with the image's new place.");
    }
    if (!e.modules_found) {
        coloured(colour::bad, "The game modules are missing. Run the setup again.");
    }
    if (e.other_version) {
        coloured(colour::warn,
                 "Another version of the program built this; run the setup again if the game does not "
                 "start.");
    }
}

void draw_session(App& app) {
    const GameSession& s = app.game().session();
    switch (s.phase()) {
    case GamePhase::idle:
        return;
    case GamePhase::starting:
        text("Starting the game...");
        break;
    case GamePhase::running: {
        ImGui::TextColored(colour::good, "The game is running");
        if (s.started() && s.started()->pid != 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(process %lld)", static_cast<long long>(s.started()->pid));
        }
        if (s.first_frame_ms()) {
            dimmed("First picture after " + std::to_string(static_cast<long long>(*s.first_frame_ms())) +
                   " ms");
        }
        if (s.fps()) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%.1f frames per second, %.0f%% speed", s.fps()->fps,
                          s.fps()->speed * 100.0);
            text(buf);
        }
        break;
    }
    case GamePhase::exited:
        if (const auto& e = s.exit()) {
            if (e->ok) {
                ImGui::TextColored(colour::good, "%s", e->problem.message.c_str());
            } else {
                problem_box(e->problem, "exit_problem");
            }
            if (e->exit_code || e->signal != 0) {
                dimmed(e->signal != 0 ? "Ended by signal " + std::to_string(e->signal)
                                      : "Exit status " + std::to_string(*e->exit_code) +
                                            (e->reason.empty() ? "" : ", reason " + e->reason));
            }
        }
        break;
    }
    if (!s.warnings().empty()) {
        subheading("Warnings");
        for (const StatusWarning& w : s.warnings()) {
            coloured(colour::warn, (w.code.empty() ? "" : w.code + ": ") + w.message);
        }
    }
    if (ImGui::CollapsingHeader("Game output")) {
        copy_button("Copy output", s.log().text());
        log_view("game_log", s.log(), std::max(160.0f, ImGui::GetContentRegionAvail().y - 8.0f));
    }
}

} // namespace

void draw_play_page(App& app) {
    LibraryModel& lib = app.library();
    GameSupervisor& game = app.game();
    heading("Play");
    ImGui::Spacing();
    if (lib.problem()) {
        problem_box(*lib.problem(), "library_problem");
        ImGui::Spacing();
    }
    if (lib.empty()) {
        wrapped("No game is set up yet. The setup builds the game from your disc image.");
        if (primary_button("Set up the game")) {
            app.go(Page::setup);
        }
        ImGui::SameLine();
        if (ImGui::Button("Look again")) {
            lib.refresh();
        }
        draw_session(app);
        return;
    }

    for (std::size_t i = 0; i < lib.entries().size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const bool selected = lib.selected() == i;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? ImVec4(0.15f, 0.19f, 0.27f, 1.0f)
                                                         : ImGui::GetStyleColorVec4(ImGuiCol_ChildBg));
        ImGui::BeginChild("entry", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        draw_entry(lib.entries()[i]);
        if (!selected && ImGui::Button("Select")) {
            lib.select(i);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::Spacing();

    const float big = ImGui::GetFrameHeight() * 1.6f;
    if (game.active()) {
        if (ImGui::Button("Stop the game", ImVec2(200.0f * ImGui::GetStyle().FontScaleDpi, big))) {
            game.stop();
        }
    } else {
        const LibraryEntry* e = lib.current();
        const bool can_play =
            e != nullptr && e->image_found && e->modules_found && (app.preview() || !app.env().game.empty());
        ImGui::BeginDisabled(!can_play);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
        if (primary_button("Play", ImVec2(200.0f * ImGui::GetStyle().FontScaleDpi, big))) {
            app.play();
        }
        ImGui::PopFont();
        ImGui::EndDisabled();
        if (!app.preview() && app.env().game.empty()) {
            coloured(colour::bad, "dq8-game was not found next to the launcher. Reinstall the program.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Set up again", ImVec2(0, big))) {
        app.go(Page::setup);
    }
    if (app.settings() == nullptr) {
        dimmed("The settings could not be read, so the game starts with its defaults.");
    }
    ImGui::Spacing();
    ImGui::Separator();
    draw_session(app);
}

} // namespace dq8::launcher::ui
