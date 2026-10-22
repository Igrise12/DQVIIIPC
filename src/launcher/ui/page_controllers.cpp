// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The Controllers page: profiles, devices, bindings with live capture, and a test view (model/controllers.h).
#include "ui/app.h"
#include "ui/widgets.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <imgui.h>

namespace dq8::launcher::ui {

namespace {

void draw_profiles(App& app, ControllerModel& m) {
    const std::string& name = m.profile_name();
    ImGui::AlignTextToFramePadding();
    text("Profile");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f * ImGui::GetStyle().FontScaleDpi);
    if (ImGui::BeginCombo("##profile", name.c_str())) {
        for (const std::string& p : m.profiles()) {
            if (ImGui::Selectable(p.c_str(), p == name)) {
                m.select_profile(p);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("New...")) {
        ImGui::OpenPopup("new_profile");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(name == "default");
    if (ImGui::Button("Delete")) {
        m.delete_profile();
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("new_profile")) {
        static char buf[65] = {};
        text("A copy of this profile, named:");
        ImGui::SetNextItemWidth(220.0f * ImGui::GetStyle().FontScaleDpi);
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        const bool entered =
            ImGui::InputText("##name", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button("Create") || entered) && m.create_profile(buf)) {
            buf[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        dimmed("Letters, digits, '_' and '-'.");
        ImGui::EndPopup();
    }

    // The game uses the profile the input.profile setting names.
    if (SettingsModel* settings = app.settings()) {
        std::string active;
        try {
            active = format_value(settings->backend().value("input.profile"));
        } catch (...) {
        }
        if (!active.empty()) {
            ImGui::SameLine();
            if (active == name) {
                ImGui::TextColored(colour::good, "The game uses this profile");
            } else if (ImGui::Button("Use this profile in the game")) {
                settings->set("input.profile", name);
            }
        }
    }
    if (!m.persistent()) {
        coloured(colour::warn,
                 "Controller profiles cannot be saved to files in this build yet; changes last until "
                 "the launcher closes.");
    }
}

void draw_devices(App& app, ControllerModel& m) {
    const std::vector<DeviceView> devices = m.devices();
    for (std::size_t i = 0; i < devices.size(); ++i) {
        const DeviceView& d = devices[i];
        std::string label = d.name;
        if (d.kind == DeviceKind::gamepad) {
            if (!d.connected) {
                label += " (not connected)";
            }
            if (d.port) {
                label += ", port " + std::to_string(*d.port + 1);
            }
        }
        label += "##device" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), m.selected_device() == i)) {
            m.select_device(i);
        }
    }
    if (devices.size() == 2) {
        dimmed("No gamepad is connected.");
    }
    const DeviceView d = m.current_device();
    if (d.kind != DeviceKind::gamepad) {
        return;
    }
    ImGui::Spacing();
    if (d.customized) {
        if (ImGui::Button("Use the bindings for all gamepads")) {
            m.forget_device();
        }
    } else if (ImGui::Button("Give this gamepad its own bindings")) {
        m.customize_device();
    }
    const char* ports[] = {"First free port", "Always port 1", "Always port 2"};
    int port = d.port ? static_cast<int>(*d.port) + 1 : 0;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##port", &port, ports, 3)) {
        m.set_port(port == 0 ? std::nullopt : std::optional<unsigned>(static_cast<unsigned>(port - 1)));
    }
    if (d.connected && ImGui::Button("Test vibration")) {
        for (SDL_Gamepad* pad : app.gamepads().with_guid(d.guid)) {
            SDL_RumbleGamepad(pad, 0xC000, 0xC000, 600);
        }
    }
}

void draw_bindings(ControllerModel& m) {
    const auto now = std::chrono::steady_clock::now();
    const DeviceView d = m.current_device();
    if (m.capturing()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s for %s... (Escape cancels, %.0f s)",
                      d.kind == DeviceKind::keyboard ? "Press a key"
                                                     : "Press a button or move a stick or trigger",
                      std::string(control_label(*m.capturing())).c_str(), m.capture_seconds_left(now));
        coloured(colour::accent, buf);
    } else if (!m.status().empty()) {
        dimmed(m.status());
    } else {
        dimmed(d.kind == DeviceKind::keyboard ? "Bind sets one key; + adds another."
                                              : "Bind sets one button or stick direction; + adds another.");
    }
    if (m.problem()) {
        problem_box(*m.problem(), "controllers_problem");
    }
    if (!ImGui::BeginTable("bindings", 3,
                           ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH |
                               ImGuiTableFlags_ScrollY,
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Bound to");
    ImGui::TableSetupColumn("Change", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (std::size_t c = 0; c < control_count; ++c) {
        ImGui::PushID(static_cast<int>(c));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        text(control_label(c));
        ImGui::TableNextColumn();
        const bool inherited = m.inherited(c);
        const SourceList sources = m.sources(c);
        if (m.capturing() == c) {
            ImGui::TextColored(colour::accent, "waiting for input...");
        } else if (sources.empty()) {
            ImGui::TextDisabled("not bound");
        }
        for (const std::string& s : sources) {
            if (m.capturing() == c) {
                break;
            }
            ImGui::BeginDisabled(inherited);
            if (ImGui::SmallButton((s + "  x##" + s).c_str())) {
                m.remove_source(c, s);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (inherited) {
                    ImGui::SetTooltip("From the bindings for all gamepads");
                } else {
                    ImGui::SetTooltip("Remove %s", s.c_str());
                }
            }
            ImGui::SameLine();
        }
        if (inherited) {
            ImGui::TextDisabled("(all gamepads)");
        }
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(m.capturing().has_value());
        if (ImGui::SmallButton("Bind")) {
            m.begin_capture(c, false, now);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("+")) {
            m.begin_capture(c, true, now);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            m.clear_control(c);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(d.kind == DeviceKind::gamepad ? "Inherit" : "Default")) {
            m.reset_control(c);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void draw_test(App& app, ControllerModel& m) {
    const DeviceView d = m.current_device();
    const auto levels =
        m.levels([&](std::string_view source) { return source_level(source, d, app.gamepads()); });
    dimmed("Press buttons to see which controls they drive.");
    const float cell = 150.0f * ImGui::GetStyle().FontScaleDpi;
    const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cell));
    if (ImGui::BeginTable("test", columns)) {
        for (std::size_t c = 0; c < control_count; ++c) {
            ImGui::TableNextColumn();
            const float level = levels[c];
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, level > 0.5f ? colour::good : colour::accent);
            ImGui::ProgressBar(level, ImVec2(-1.0f, 0.0f), std::string(control_label(c)).c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
}

} // namespace

void draw_controllers_page(App& app) {
    ControllerModel& m = app.controllers();
    heading("Controllers");
    draw_profiles(app, m);
    ImGui::Spacing();
    const float left = 260.0f * ImGui::GetStyle().FontScaleDpi;
    ImGui::BeginChild("devices", ImVec2(left, 0), ImGuiChildFlags_Borders);
    subheading("Devices");
    draw_devices(app, m);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("right", ImVec2(0, 0));
    if (ImGui::BeginTabBar("controller_tabs")) {
        if (ImGui::BeginTabItem("Bindings")) {
            draw_bindings(m);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Test")) {
            draw_test(app, m);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

} // namespace dq8::launcher::ui
