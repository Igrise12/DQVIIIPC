// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The setup's event format (dq8/core/setup.h); run() is the M5 day-0 stub that M5 session A replaces.
#include "dq8/core/setup.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace dq8::core::setup {

namespace {

void append_json_string(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

} // namespace

std::string_view to_string(Stage stage) noexcept {
    switch (stage) {
    case Stage::verify:
        return "verify";
    case Stage::extract:
        return "extract";
    case Stage::toolchain:
        return "toolchain";
    case Stage::maps:
        return "maps";
    case Stage::prepare:
        return "prepare";
    case Stage::generate:
        return "generate";
    case Stage::build:
        return "build";
    case Stage::smoke:
        return "smoke";
    case Stage::register_install:
        return "register";
    }
    return "unknown";
}

std::string_view to_string(State state) noexcept {
    switch (state) {
    case State::start:
        return "start";
    case State::progress:
        return "progress";
    case State::done:
        return "done";
    case State::skipped:
        return "skipped";
    case State::error:
        return "error";
    }
    return "unknown";
}

std::string to_json_line(const Event& event) {
    std::string out = R"({"ev":"stage","stage":)";
    append_json_string(out, to_string(event.stage));
    out += R"(,"state":)";
    append_json_string(out, to_string(event.state));
    out += R"(,"done":)" + std::to_string(event.done);
    out += R"(,"total":)" + std::to_string(event.total);
    if (!event.unit.empty()) {
        out += R"(,"unit":)";
        append_json_string(out, event.unit);
    }
    if (!event.message.empty()) {
        out += R"(,"message":)";
        append_json_string(out, event.message);
    }
    if (event.code) {
        out += R"(,"code":)";
        append_json_string(out, core::to_string(*event.code));
    }
    out += '}';
    return out;
}

Result run(const Options& /*options*/) {
    throw std::logic_error("setup: run is not implemented (M5 session A)");
}

} // namespace dq8::core::setup
