// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "host/event_log.h"

#include "dq8/core/error.h"
#include "host/json_writer.h"

#include <variant>

namespace dq8::host {

EventLog::EventLog(const std::optional<std::string>& target, bool trace_in_stream)
    : trace_in_stream_(trace_in_stream) {
    if (!target) {
        return;
    }
    if (*target == "-") {
        out_ = stdout;
        return;
    }
    file_ = std::make_unique<std::ofstream>(fs::path(*target), std::ios::binary | std::ios::trunc);
    if (!*file_) {
        throw core::Error(core::ErrorCode::io_write, "cannot create the event file " + *target);
    }
}

EventLog::~EventLog() {
    flush();
}

void EventLog::flush() {
    if (file_) {
        file_->flush();
    }
    if (out_ != nullptr) {
        std::fflush(out_);
    }
}

std::string EventLog::format_trace(const rt::MachineEvent& event, std::uint64_t vblank) {
    std::string cat;
    std::string msg;
    for (const auto& [key, value] : event.fields) {
        const auto* text = std::get_if<std::string>(&value);
        if (text != nullptr && key == "cat") {
            cat = *text;
        } else if (text != nullptr && key == "msg") {
            msg = *text;
        }
    }
    return "trace " + std::to_string(vblank) + " [" + cat + "] " + msg;
}

std::string EventLog::format(const rt::MachineEvent& event, std::uint64_t vblank, std::uint64_t cycles) {
    JsonWriter j;
    j.begin_object();
    j.member("ev", event.type);
    j.member("vblank", vblank);
    j.member("cycles", cycles);
    for (const auto& [key, value] : event.fields) {
        j.key(key);
        std::visit([&j](const auto& v) { j.value(v); }, value);
    }
    j.end_object();
    return j.str();
}

void EventLog::write(const rt::MachineEvent& event, std::uint64_t vblank, std::uint64_t cycles) {
    const bool trace = event.type == "trace";
    if (trace) {
        std::fprintf(stderr, "%s\n", format_trace(event, vblank).c_str());
    }
    if (!enabled() || (trace && !trace_in_stream_)) {
        return;
    }
    const std::string line = format(event, vblank, cycles);
    if (file_) {
        *file_ << line << '\n';
    } else if (out_ != nullptr) {
        std::fputs(line.c_str(), out_);
        std::fputc('\n', out_);
    }
}

} // namespace dq8::host
