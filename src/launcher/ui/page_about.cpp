// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The About page: version, licence, the bundled font, and THIRD_PARTY_NOTICES.md.
#include "dq8/core/version.h"
#include "ui/app.h"
#include "ui/widgets.h"

#include <imgui.h>

namespace dq8::launcher::ui {

namespace {

void draw_markdown(const std::vector<MdBlock>& blocks) {
    int table_id = 0;
    for (const MdBlock& b : blocks) {
        switch (b.kind) {
        case MdBlock::Kind::heading:
            ImGui::Spacing();
            if (b.level <= 1) {
                heading(b.text, 1.25f);
            } else {
                subheading(b.text);
            }
            break;
        case MdBlock::Kind::paragraph:
            wrapped(b.text);
            break;
        case MdBlock::Kind::bullet:
            ImGui::Bullet();
            ImGui::SameLine();
            wrapped(b.text);
            break;
        case MdBlock::Kind::rule:
            ImGui::Separator();
            break;
        case MdBlock::Kind::table: {
            if (b.rows.empty()) {
                break;
            }
            const int columns = static_cast<int>(b.rows.front().size());
            ImGui::PushID(table_id++);
            if (columns > 0 && ImGui::BeginTable("md", columns,
                                                 ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp |
                                                     ImGuiTableFlags_RowBg)) {
                for (const std::string& h : b.rows.front()) {
                    ImGui::TableSetupColumn(h.c_str());
                }
                ImGui::TableHeadersRow();
                for (std::size_t r = 1; r < b.rows.size(); ++r) {
                    ImGui::TableNextRow();
                    for (int c = 0; c < columns; ++c) {
                        ImGui::TableNextColumn();
                        if (static_cast<std::size_t>(c) < b.rows[r].size()) {
                            wrapped(b.rows[r][static_cast<std::size_t>(c)]);
                        }
                    }
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
            break;
        }
        }
    }
}

} // namespace

void draw_about_page(App& app) {
    heading("DQ8-Native");
    dimmed(std::string("Version ") + core::version());
    ImGui::Spacing();
    wrapped("A native PC port of Dragon Quest VIII (PlayStation 2) by static recompilation. It builds the "
            "game from your own disc image on your computer; the program contains nothing from the game.");
    wrapped("Dragon Quest is a trademark of its owners. This project is not affiliated with or endorsed "
            "by them.");
    ImGui::Spacing();
    subheading("Licence");
    wrapped("DQ8-Native is free software: you can redistribute it and modify it under the terms of the GNU "
            "General Public License, version 3 or (at your option) any later version. It comes with "
            "ABSOLUTELY NO WARRANTY. The full text is in the LICENSE file shipped with the program.");
    wrapped("The interface uses the Noto Sans font (Copyright 2022 The Noto Project Authors), under the "
            "SIL Open Font License 1.1.");
    ImGui::Spacing();
    subheading("Third-party software");
    if (app.notices().empty()) {
        dimmed("THIRD_PARTY_NOTICES.md was not found next to the program.");
        return;
    }
    dimmed("From " + app.notices_file());
    ImGui::BeginChild("notices", ImVec2(0, 0), ImGuiChildFlags_Borders);
    draw_markdown(app.notices());
    ImGui::EndChild();
}

} // namespace dq8::launcher::ui
