// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The first-run wizard's page: draws the Wizard's state (model/wizard.h).
#include "ui/app.h"
#include "ui/widgets.h"

#include <algorithm>
#include <cstdio>
#include <imgui.h>

namespace dq8::launcher::ui {

namespace {

using core::setup::Stage;

int step_number(WizardStep step) {
    switch (step) {
    case WizardStep::choose_image:
    case WizardStep::verifying:
    case WizardStep::verify_failed:
        return 1;
    case WizardStep::toolchain_checking:
    case WizardStep::toolchain:
    case WizardStep::ready:
        return 2;
    case WizardStep::running:
    case WizardStep::cancelling:
    case WizardStep::cancelled:
    case WizardStep::failed:
        return 3;
    case WizardStep::finished:
        return 4;
    }
    return 1;
}

void draw_steps(WizardStep step) {
    static const char* names[] = {"Disc image", "Build tools", "Setup", "Ready"};
    const int current = step_number(step);
    for (int i = 0; i < 4; ++i) {
        if (i != 0) {
            ImGui::SameLine();
            ImGui::TextDisabled(">");
            ImGui::SameLine();
        }
        const std::string label = std::to_string(i + 1) + ". " + names[i];
        if (i + 1 < current) {
            ImGui::TextColored(colour::good, "%s", label.c_str());
        } else if (i + 1 == current) {
            ImGui::TextColored(colour::accent, "%s", label.c_str());
        } else {
            ImGui::TextDisabled("%s", label.c_str());
        }
    }
    ImGui::Separator();
}

void draw_choose(App& app, Wizard& w) {
    wrapped(
        "DQ8-Native builds the game from your own copy of Dragon Quest VIII for the PlayStation 2 (NTSC-U). "
        "Choose a disc image of it (an .iso file). The image is checked first; nothing is changed in it.");
    ImGui::Spacing();
    if (primary_button("Choose disc image...")) {
        app.open_dialog(DialogPurpose::disc_image);
    }
    const LauncherState& state = app.state();
    if (!state.last_image.empty()) {
        ImGui::SameLine();
        const std::string label = "Use " + to_utf8(state.last_image.filename());
        if (ImGui::Button(label.c_str())) {
            w.choose_image(state.last_image);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", to_utf8(state.last_image).c_str());
        }
    }
    ImGui::Spacing();
    dimmed("Or type the image's path:");
    static char path[1024] = {};
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Use this path").x -
                            ImGui::GetStyle().FramePadding.x * 4.0f);
    const bool entered =
        ImGui::InputText("##image_path", path, sizeof(path), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Use this path") || entered) && path[0] != '\0') {
        app.state().last_image = from_utf8(path);
        app.save_state();
        w.choose_image(from_utf8(path));
    }
}

void draw_toolchain(Wizard& w) {
    if (const auto& report = w.toolchain()) {
        const auto tool_row = [](const char* name, const core::toolchain::Tool& t) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            text(name);
            ImGui::TableNextColumn();
            if (t.found) {
                ImGui::TextColored(colour::good, "%s", t.version.empty() ? "found" : t.version.c_str());
                ImGui::TableNextColumn();
                dimmed(to_utf8(t.path));
            } else {
                ImGui::TextColored(colour::bad, "missing");
                ImGui::TableNextColumn();
            }
        };
        if (ImGui::BeginTable("tools", 3,
                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Where");
            tool_row("C compiler", report->toolchain.cc);
            tool_row("C++ compiler", report->toolchain.cxx);
            tool_row("CMake", report->toolchain.cmake);
            tool_row("Ninja", report->toolchain.ninja);
            ImGui::EndTable();
        }
        for (const core::toolchain::Problem& p : report->problems) {
            Problem shown = explain(p.code, {});
            shown.message = "Missing: " + p.tool;
            shown.action = p.advice;
            problem_box(shown, ("problem_" + p.tool).c_str());
        }
    }
}

void draw_ready(Wizard& w) {
    if (const auto& v = w.verified()) {
        ImGui::TextColored(colour::good, "Disc verified");
        ImGui::SameLine();
        text(v->title.empty() ? v->profile_id : v->title + " (" + v->profile_id + ")");
        for (const std::string& note : v->warnings) {
            dimmed("Note: " + note);
        }
    }
    draw_toolchain(w);
}

void draw_progress(App& app, Wizard& w) {
    const SetupProgress& p = w.progress();
    char overlay[96];
    std::snprintf(overlay, sizeof(overlay), "%d%%", static_cast<int>(p.overall() * 100.0));
    ImGui::ProgressBar(static_cast<float>(p.overall()), ImVec2(-1.0f, 0.0f), overlay);
    if (const auto left = p.remaining(w.now()); left && w.step() == WizardStep::running) {
        dimmed("About " + human_duration(*left) + " left in this stage");
    }
    if (ImGui::BeginTable("stages", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Progress");
        for (const StageProgress& s : p.stages()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            switch (s.state) {
            case StageState::pending:
                ImGui::TextDisabled("waiting");
                break;
            case StageState::running:
                ImGui::TextColored(colour::accent, "running");
                break;
            case StageState::done:
                ImGui::TextColored(colour::good, "done");
                break;
            case StageState::skipped:
                ImGui::TextColored(colour::good, "up to date");
                break;
            case StageState::failed:
                ImGui::TextColored(colour::bad, "failed");
                break;
            }
            ImGui::TableNextColumn();
            text(stage_title(s.stage));
            ImGui::TableNextColumn();
            if (s.state == StageState::running) {
                if (const auto f = s.fraction()) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%llu / %llu", static_cast<unsigned long long>(s.done),
                                  static_cast<unsigned long long>(s.total));
                    ImGui::ProgressBar(static_cast<float>(*f), ImVec2(-1.0f, 0.0f), buf);
                } else {
                    // Unknown total: a bar that moves so the stage visibly lives.
                    const float t = static_cast<float>(ImGui::GetTime());
                    ImGui::ProgressBar(-t * 0.5f, ImVec2(-1.0f, 0.0f), "working");
                }
                if (!s.unit.empty() || !s.message.empty()) {
                    dimmed(s.unit.empty() ? s.message : s.unit + (s.message.empty() ? "" : ": " + s.message));
                }
            } else if (s.state == StageState::failed && !s.message.empty()) {
                dimmed(s.message);
            }
        }
        ImGui::EndTable();
    }
    (void)app;
}

} // namespace

void draw_setup_page(App& app) {
    Wizard& w = app.wizard();
    heading("Set up the game");
    draw_steps(w.step());
    ImGui::Spacing();

    switch (w.step()) {
    case WizardStep::choose_image:
        draw_choose(app, w);
        break;
    case WizardStep::verifying:
        text("Checking " + to_utf8(w.image().filename()) + " against the supported releases...");
        ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()) * 0.5f, ImVec2(-1.0f, 0.0f), "verifying");
        break;
    case WizardStep::verify_failed:
        if (w.problem()) {
            problem_box(*w.problem(), "verify_problem");
        }
        ImGui::Spacing();
        draw_choose(app, w);
        break;
    case WizardStep::toolchain_checking:
        draw_ready(w);
        text("Looking for the compiler, CMake and Ninja...");
        ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()) * 0.5f, ImVec2(-1.0f, 0.0f), "checking");
        break;
    case WizardStep::toolchain:
    case WizardStep::ready: {
        draw_ready(w);
        if (w.problem()) {
            problem_box(*w.problem(), "toolchain_problem");
        }
        ImGui::Spacing();
        if (w.step() == WizardStep::ready) {
            wrapped(
                "Everything is in place. The setup translates the game's code to C++ and compiles it on this "
                "computer; the first time takes 15 to 40 minutes. You can cancel it and continue later.");
        } else {
            wrapped("Install what is missing, then check again. You can also start the setup now; it stops "
                    "at the "
                    "build tools if they are still missing.");
        }
        ImGui::Spacing();
        int jobs = static_cast<int>(w.jobs() == 0 ? w.default_jobs() : w.jobs());
        const int max_jobs = static_cast<int>(std::max(2u, w.default_jobs() * 2));
        ImGui::SetNextItemWidth(220.0f * ImGui::GetStyle().FontScaleDpi);
        if (ImGui::SliderInt("Parallel compile jobs", &jobs, 1, max_jobs)) {
            w.set_jobs(static_cast<unsigned>(jobs) == w.default_jobs() ? 0u : static_cast<unsigned>(jobs));
        }
        bool force = w.force();
        if (ImGui::Checkbox("Rebuild everything, even what is up to date", &force)) {
            w.set_force(force);
        }
        ImGui::Spacing();
        if (w.step() == WizardStep::ready ? primary_button("Start setup")
                                          : ImGui::Button("Start setup anyway")) {
            app.state().last_image = w.image();
            app.save_state();
            w.start_setup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Check again")) {
            w.check_toolchain();
        }
        ImGui::SameLine();
        if (ImGui::Button("Choose another image")) {
            w.restart();
        }
        break;
    }
    case WizardStep::running:
    case WizardStep::cancelling:
        draw_progress(app, w);
        ImGui::Spacing();
        ImGui::BeginDisabled(w.step() == WizardStep::cancelling);
        if (ImGui::Button(w.step() == WizardStep::cancelling ? "Cancelling..." : "Cancel")) {
            w.cancel();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        dimmed("Cancelling stops at a safe point; the next run continues where it stopped.");
        break;
    case WizardStep::cancelled:
    case WizardStep::failed:
        if (w.problem()) {
            problem_box(*w.problem(), "setup_problem");
        }
        ImGui::Spacing();
        draw_progress(app, w);
        if (primary_button(w.step() == WizardStep::cancelled ? "Resume setup" : "Try again")) {
            w.resume();
        }
        ImGui::SameLine();
        if (ImGui::Button("Choose another image")) {
            w.restart();
        }
        break;
    case WizardStep::finished:
        draw_progress(app, w);
        ImGui::TextColored(colour::good, "The game is set up.");
        if (primary_button("Go to Play")) {
            app.go(Page::play);
        }
        break;
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Log", w.busy() ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None)) {
        copy_button("Copy log", w.log().text());
        log_view("setup_log", w.log(), std::max(160.0f, ImGui::GetContentRegionAvail().y - 8.0f));
    }
}

} // namespace dq8::launcher::ui
