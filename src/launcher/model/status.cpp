// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "model/status.h"

#include <nlohmann/json.hpp>

namespace dq8::launcher {

namespace {

using json = nlohmann::json;

std::string string_field(const json& j, const char* name) {
    const auto it = j.find(name);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::optional<double> number_field(const json& j, const char* name) {
    const auto it = j.find(name);
    if (it == j.end() || !it->is_number()) {
        return std::nullopt;
    }
    return it->get<double>();
}

std::optional<std::int64_t> integer_field(const json& j, const char* name) {
    const auto it = j.find(name);
    if (it == j.end() || !it->is_number_integer()) {
        return std::nullopt;
    }
    return it->get<std::int64_t>();
}

} // namespace

std::optional<StatusEvent> parse_status_line(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.remove_suffix(1);
    }
    while (!line.empty() && line.front() == ' ') {
        line.remove_prefix(1);
    }
    if (line.empty() || line.front() != '{') {
        return std::nullopt; // the cheap test first: most output is not JSON
    }
    const json j = json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        return std::nullopt;
    }
    const auto ev = j.find("ev");
    if (ev == j.end() || !ev->is_string()) {
        return std::nullopt;
    }
    const std::string name = ev->get<std::string>();
    if (name == "started") {
        StatusStarted s;
        s.version = string_field(j, "version");
        s.profile = string_field(j, "profile");
        s.pid = integer_field(j, "pid").value_or(0);
        const auto w = j.find("window");
        s.window = w != j.end() && w->is_boolean() && w->get<bool>();
        return s;
    }
    if (name == "first_frame") {
        return StatusFirstFrame{number_field(j, "ms").value_or(0.0)};
    }
    if (name == "fps") {
        return StatusFps{number_field(j, "fps").value_or(0.0), number_field(j, "speed").value_or(0.0)};
    }
    if (name == "warning") {
        return StatusWarning{string_field(j, "code"), string_field(j, "message")};
    }
    if (name == "exit") {
        StatusExit e;
        if (const auto status = integer_field(j, "status")) {
            e.status = static_cast<int>(*status);
        }
        e.reason = string_field(j, "reason");
        e.code = string_field(j, "code");
        e.message = string_field(j, "message");
        return e;
    }
    return StatusOther{name};
}

void LineSplitter::feed(std::string_view chunk, const LineFn& on_line) {
    while (!chunk.empty()) {
        const std::size_t nl = chunk.find('\n');
        const std::string_view piece = chunk.substr(0, nl);
        if (nl != std::string_view::npos) {
            if (pending_.empty() && piece.size() <= max_line_) {
                on_line(piece);
            } else {
                pending_ += piece;
                while (pending_.size() > max_line_) {
                    on_line(std::string_view(pending_).substr(0, max_line_));
                    pending_.erase(0, max_line_);
                }
                on_line(pending_);
                pending_.clear();
            }
            chunk.remove_prefix(nl + 1);
            continue;
        }
        pending_ += piece;
        while (pending_.size() > max_line_) {
            on_line(std::string_view(pending_).substr(0, max_line_));
            pending_.erase(0, max_line_);
        }
        break;
    }
}

void LineSplitter::finish(const LineFn& on_line) {
    if (!pending_.empty()) {
        on_line(pending_);
        pending_.clear();
    }
}

} // namespace dq8::launcher
