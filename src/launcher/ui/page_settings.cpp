// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The Settings pages: one tab per schema category, one row per setting, the widget from the SettingsModel. No
// setting is named here: a new schema row appears without a change to this file.
#include "ui/app.h"
#include "ui/widgets.h"

#include <algorithm>
#include <cstring>
#include <imgui.h>

namespace dq8::launcher::ui {

namespace {

using core::settings::Value;

void draw_control(App& app, SettingsModel& m, const SettingRow& r) {
    const std::string id = "##" + r.key;
    ImGui::SetNextItemWidth(-1.0f);
    switch (r.widget) {
    case Widget::checkbox: {
        bool v = std::get<bool>(r.value);
        if (ImGui::Checkbox(id.c_str(), &v)) {
            m.set(r.key, v);
        }
        break;
    }
    case Widget::slider_int: {
        int v = static_cast<int>(std::get<std::int64_t>(r.value));
        if (ImGui::SliderInt(id.c_str(), &v, static_cast<int>(r.min), static_cast<int>(r.max))) {
            m.set(r.key, std::int64_t{v});
        }
        break;
    }
    case Widget::input_int: {
        int v = static_cast<int>(std::get<std::int64_t>(r.value));
        // Each edit is stored (clamped to the range); the file is written once the field is left.
        if (ImGui::InputInt(id.c_str(), &v, 1, 10)) {
            m.set(r.key, std::int64_t{v});
        }
        break;
    }
    case Widget::slider_real: {
        float v = static_cast<float>(std::get<double>(r.value));
        if (ImGui::SliderFloat(id.c_str(), &v, static_cast<float>(r.min_real), static_cast<float>(r.max_real),
                               "%.2f")) {
            m.set(r.key, static_cast<double>(v));
        }
        break;
    }
    case Widget::combo: {
        const std::string& current = std::get<std::string>(r.value);
        std::string shown = current;
        for (const Choice& c : r.choices) {
            if (c.value == current) {
                shown = c.text;
            }
        }
        if (ImGui::BeginCombo(id.c_str(), shown.c_str())) {
            for (const Choice& c : r.choices) {
                const bool selected = c.value == current;
                if (ImGui::Selectable(c.text.c_str(), selected)) {
                    m.set(r.key, c.value);
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case Widget::text:
    case Widget::path: {
        char buf[1024];
        const std::string& v = std::get<std::string>(r.value);
        const std::size_t n = std::min(v.size(), sizeof(buf) - 1);
        std::memcpy(buf, v.data(), n);
        buf[n] = '\0';
        if (r.widget == Widget::path) {
            const float browse = ImGui::CalcTextSize("Choose...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetNextItemWidth(-browse - ImGui::GetStyle().ItemSpacing.x);
        }
        if (ImGui::InputTextWithHint(id.c_str(), r.widget == Widget::path ? "(the default folder)" : "", buf,
                                     sizeof(buf))) {
            m.set(r.key, std::string(buf));
        }
        if (r.widget == Widget::path) {
            ImGui::SameLine();
            if (ImGui::Button(("Choose..." + id).c_str())) {
                app.open_dialog(DialogPurpose::setting_folder, r.key);
            }
        }
        break;
    }
    }
}

void draw_rows(App& app, SettingsModel& m, const SettingsPage& page) {
    if (!ImGui::BeginTable("rows", 2,
                           ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_PadOuterX)) {
        return;
    }
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 1.1f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    for (const SettingRow& r : page.rows) {
        ImGui::PushID(r.key.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        text(r.label);
        if (r.advanced) {
            ImGui::SameLine();
            ImGui::TextDisabled("(advanced)");
        }
        if (!r.description.empty()) {
            dimmed(r.description);
        }
        if (!r.hint.empty()) {
            coloured(colour::warn, r.hint);
        }
        ImGui::TableNextColumn();
        draw_control(app, m, r);
        if (r.modified) {
            if (ImGui::SmallButton("Reset to default")) {
                m.reset(r.key);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("default: %s", format_value(r.default_value).c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

} // namespace

void draw_settings_page(App& app) {
    heading("Settings");
    SettingsModel* m = app.settings();
    if (m == nullptr) {
        ImGui::Spacing();
        wrapped("The settings are not available.");
        if (app.settings_problem()) {
            problem_box(*app.settings_problem(), "settings_problem");
        }
        return;
    }
    bool advanced = app.state().show_advanced;
    if (ImGui::Checkbox("Show advanced settings", &advanced)) {
        app.state().show_advanced = advanced;
    }
    ImGui::SameLine();
    dimmed("Saved to " + to_utf8(m->file()));
    if (app.game().active()) {
        coloured(colour::warn, "The game is running: changes apply the next time it starts.");
    }
    if (m->problem()) {
        problem_box(*m->problem(), "settings_save_problem");
    }
    for (const std::string& w : m->warnings()) {
        coloured(colour::warn, w);
    }
    if (ImGui::BeginTabBar("categories")) {
        for (const std::string& category : m->categories()) {
            const SettingsPage page = m->page(category, advanced, app.game().active());
            if (ImGui::BeginTabItem((page.title + "##" + category).c_str())) {
                ImGui::Spacing();
                if (page.rows.empty()) {
                    dimmed("Nothing to set here with the current choices.");
                }
                draw_rows(app, *m, page);
                ImGui::Spacing();
                if (page.advanced_hidden != 0) {
                    dimmed(std::to_string(page.advanced_hidden) + " advanced setting" +
                           (page.advanced_hidden == 1 ? " is" : "s are") + " hidden.");
                }
                if (ImGui::Button("Reset this page")) {
                    m->reset_page(category);
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

} // namespace dq8::launcher::ui
