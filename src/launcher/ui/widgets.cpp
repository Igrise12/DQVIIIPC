// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "ui/widgets.h"

#include <SDL3/SDL.h>
#include <cstdio>

namespace dq8::launcher::ui {

void apply_style(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 0.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.GrabRounding = 5.0f;
    style.TabRounding = 5.0f;
    style.FramePadding = ImVec2(9.0f, 6.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.WindowPadding = ImVec2(16.0f, 14.0f);
    style.ScrollbarSize = 14.0f;
    style.FontSizeBase = 17.0f;
    // Plain geometry for anti-aliased lines: SDL's software renderer (no GPU, the dummy driver) loses the
    // textured ones, and a launcher draws few enough lines for the cost not to matter.
    style.AntiAliasedLinesUseTex = false;
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.13f, 0.16f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.13f, 0.16f, 0.98f);
    c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.20f, 0.24f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.27f, 0.33f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.32f, 0.40f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.23f, 0.29f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.32f, 0.42f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.40f, 0.58f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.22f, 0.30f, 0.44f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.27f, 0.36f, 0.52f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.42f, 0.62f, 1.0f);
    c[ImGuiCol_CheckMark] = colour::accent;
    c[ImGuiCol_SliderGrab] = colour::accent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.66f, 0.98f, 1.0f);
    c[ImGuiCol_PlotHistogram] = colour::accent;
    c[ImGuiCol_Tab] = ImVec4(0.16f, 0.18f, 0.22f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(0.27f, 0.36f, 0.52f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.22f, 0.30f, 0.44f, 1.0f);
    c[ImGuiCol_NavCursor] = ImVec4(0.55f, 0.75f, 1.0f, 1.0f);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

void heading(std::string_view t, float size) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * size);
    text(t);
    ImGui::PopFont();
}

void subheading(std::string_view t) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
    text(t);
    ImGui::PopFont();
}

void text(std::string_view t) {
    ImGui::TextUnformatted(t.data(), t.data() + t.size());
}

void wrapped(std::string_view t) {
    ImGui::PushTextWrapPos(0.0f);
    text(t);
    ImGui::PopTextWrapPos();
}

void dimmed(std::string_view t) {
    ImGui::PushStyleColor(ImGuiCol_Text, colour::dim);
    wrapped(t);
    ImGui::PopStyleColor();
}

void coloured(const ImVec4& c, std::string_view t) {
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    wrapped(t);
    ImGui::PopStyleColor();
}

void problem_box(const Problem& p, const char* id) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.25f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, colour::bad);
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    coloured(colour::bad, p.message.empty() ? std::string_view("Something went wrong.") : p.message);
    if (!p.action.empty()) {
        wrapped(p.action);
    }
    if (!p.detail.empty() && p.detail != p.message) {
        dimmed(p.detail);
    }
    if (!p.code_name.empty()) {
        dimmed("Code: " + p.code_name);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

bool primary_button(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.45f, 0.80f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.54f, 0.90f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.38f, 0.70f, 1.0f));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

void log_view(const char* id, const LogBuffer& log, float height) {
    ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const bool at_end = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    if (log.dropped() != 0) {
        dimmed("(" + std::to_string(log.dropped()) + " earlier lines are not shown)");
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(log.lines().size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            text(log.lines()[static_cast<std::size_t>(i)]);
        }
    }
    clipper.End();
    if (at_end) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void copy_button(const char* label, const std::string& t) {
    if (ImGui::SmallButton(label)) {
        SDL_SetClipboardText(t.c_str());
    }
}

std::string human_bytes(std::uint64_t bytes) {
    const char* units[] = {"bytes", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    std::size_t u = 0;
    while (v >= 1024.0 && u + 1 < sizeof(units) / sizeof(units[0])) {
        v /= 1024.0;
        ++u;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

std::string human_duration(std::chrono::seconds s) {
    const long long total = s.count();
    char buf[64];
    if (total >= 3600) {
        std::snprintf(buf, sizeof(buf), "%lld h %02lld min", total / 3600, (total % 3600) / 60);
    } else if (total >= 60) {
        std::snprintf(buf, sizeof(buf), "%lld min %02lld s", total / 60, total % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%lld s", total);
    }
    return buf;
}

} // namespace dq8::launcher::ui
