// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "host/event_log.h"

#include "dq8/core/error.h"
#include "host/json_writer.h"

#include <variant>

namespace dq8::host {

EventLog::EventLog(const std::optional<std::string>& target, std::uint32_t trace_mask) : trace_(trace_mask) {
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

std::uint32_t EventLog::category(std::string_view type) noexcept {
    const auto starts = [type](std::string_view prefix) { return type.substr(0, prefix.size()) == prefix; };
    if (starts("kernel") || starts("thread") || starts("intc") || starts("alarm")) {
        return trace_kernel;
    }
    if (starts("sif")) {
        return trace_sif;
    }
    if (starts("rpc") || starts("iop")) {
        return trace_rpc;
    }
    if (starts("dma") || starts("vif")) {
        return trace_dma;
    }
    if (starts("gs") || starts("vsync") || starts("frame")) {
        return trace_gs;
    }
    return 0;
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
    if (!enabled()) {
        return;
    }
    const std::string line = format(event, vblank, cycles);
    if (file_) {
        *file_ << line << '\n';
    } else if (out_ != nullptr) {
        std::fputs(line.c_str(), out_);
        std::fputc('\n', out_);
    }
    if ((trace_ & category(event.type)) != 0) {
        std::fprintf(stderr, "trace: %s\n", line.c_str());
    }
}

} // namespace dq8::host
