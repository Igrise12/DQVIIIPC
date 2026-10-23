// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The Diagnostics page: the support bundle (dq8/core/diagnostics.h), the facts the launcher itself knows, and
// the logs.
#include "dq8/core/version.h"
#include "ui/app.h"
#include "ui/widgets.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <imgui.h>
#include <system_error>

namespace dq8::launcher::ui {

namespace {

std::string system_report(App& app) {
    const Environment& env = app.env();
    const auto path = [](const std::filesystem::path& p) {
        return p.empty() ? std::string("(not found)") : to_utf8(p);
    };
    std::string r;
    const auto line = [&r](const std::string& k, const std::string& v) { r += k + ": " + v + "\n"; };
    line("Launcher version", core::version());
    line("Platform", SDL_GetPlatform());
    line("Logical processors", std::to_string(SDL_GetNumLogicalCPUCores()));
    line("Memory", std::to_string(SDL_GetSystemRAM()) + " MiB");
    const char* video = SDL_GetCurrentVideoDriver();
    line("Video driver", video != nullptr ? video : "(none)");
    const char* renderer = SDL_GetRendererName(app.renderer());
    line("Launcher renderer", renderer != nullptr ? renderer : "(none)");
    char scale[32];
    std::snprintf(scale, sizeof(scale), "%.2f", static_cast<double>(SDL_GetWindowDisplayScale(app.window())));
    line("Display scale", scale);
    line("dq8-game", path(env.game));
    line("dq8ctl", path(env.ctl));
    line("Configuration folder", path(env.config_dir));
    line("Data folder", path(env.data_dir));
    if (SettingsModel* s = app.settings()) {
        line("Settings file", path(s->file()));
    }
    return r;
}

} // namespace

void draw_diagnostics_page(App& app) {
    DiagnosticsModel& d = app.diagnostics();
    heading("Diagnostics");
    wrapped(
        "A support bundle collects what helps to find a problem: system information, the build tools, your "
        "settings, the logs and the build record. It never contains anything from the game.");
    ImGui::Spacing();
    ImGui::BeginDisabled(d.state() == ExportState::running);
    if (primary_button("Export support bundle...")) {
        app.open_dialog(DialogPurpose::diagnostics_file);
    }
    ImGui::SameLine();
    if (ImGui::Button("Export to the data folder") && !app.env().data_dir.empty()) {
        const std::filesystem::path dir = app.env().data_dir / "diagnostics";
        if (!app.preview()) {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec); // a failure shows as the export's own error
        }
        d.start(dir / default_diagnostics_name(std::time(nullptr)));
    }
    ImGui::EndDisabled();
    switch (d.state()) {
    case ExportState::idle:
        break;
    case ExportState::running:
        text("Collecting...");
        break;
    case ExportState::done:
        ImGui::TextColored(colour::good, "Written to %s", to_utf8(d.file()).c_str());
        ImGui::SameLine();
        copy_button("Copy path", to_utf8(d.file()));
        break;
    case ExportState::failed:
        if (d.problem()) {
            problem_box(*d.problem(), "export_problem");
        }
        break;
    }
    if (!d.log().lines().empty()) {
        log_view("export_log", d.log(), 90.0f * ImGui::GetStyle().FontScaleDpi);
    }

    ImGui::Spacing();
    subheading("This computer");
    const std::string report = system_report(app);
    ImGui::BeginChild("system", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    text(report);
    ImGui::EndChild();
    copy_button("Copy", report);

    ImGui::Spacing();
    subheading("Logs");
    if (ImGui::BeginTabBar("logs")) {
        const float height =
            std::max(160.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.0f);
        if (ImGui::BeginTabItem("Setup")) {
            copy_button("Copy log", app.wizard().log().text());
            log_view("setup_log_diag", app.wizard().log(), height);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game")) {
            copy_button("Copy log", app.game().session().log().text());
            log_view("game_log_diag", app.game().session().log(), height);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace dq8::launcher::ui
