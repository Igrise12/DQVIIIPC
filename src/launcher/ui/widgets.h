// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "model/errors.h"
#include "model/log_buffer.h"

#include <chrono>
#include <cstdint>
#include <imgui.h>
#include <string>
#include <string_view>

// Small drawing helpers the pages share. Drawing only: no decisions (those are the view-models').
namespace dq8::launcher::ui {

// The launcher's colours over Dear ImGui's dark style, scaled for the display.
void apply_style(float scale);

namespace colour {
inline constexpr ImVec4 accent{0.33f, 0.56f, 0.92f, 1.0f};
inline constexpr ImVec4 good{0.40f, 0.78f, 0.45f, 1.0f};
inline constexpr ImVec4 warn{0.95f, 0.75f, 0.30f, 1.0f};
inline constexpr ImVec4 bad{0.93f, 0.38f, 0.35f, 1.0f};
inline constexpr ImVec4 dim{0.60f, 0.62f, 0.66f, 1.0f};
} // namespace colour

void heading(std::string_view text, float size = 1.45f);
void subheading(std::string_view text);
void text(std::string_view text);
void wrapped(std::string_view text);
void dimmed(std::string_view text); // wrapped, in the dim colour
void coloured(const ImVec4& colour, std::string_view text);
// A clear frame around what went wrong and what to do.
void problem_box(const Problem& problem, const char* id);
// A filled button in the accent colour (the page's main action).
bool primary_button(const char* label, const ImVec2& size = ImVec2(0, 0));
// A log with a clipper; follows the end while it is scrolled to the bottom.
void log_view(const char* id, const LogBuffer& log, float height);
// A button that copies `text` to the clipboard.
void copy_button(const char* label, const std::string& text);

std::string human_bytes(std::uint64_t bytes);
std::string human_duration(std::chrono::seconds s);

} // namespace dq8::launcher::ui
