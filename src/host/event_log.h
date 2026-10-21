// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "host/options.h"
#include "runtime/machine/machine_services.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// dq8-game's JSON-lines event stream (architecture §11.1, §16; ADR-0007): one object per line,
// {"ev": type, "vblank": N, "cycles": C, ...fields}. Only deterministic fields go in (no wall time, no host
// pointers), so two runs of the same disc, build and input give byte-identical streams. Written on the
// machine thread only. "trace" events (MachineServices::trace, emitted only for the categories --trace turns
// on) go to stderr as one line each, and into the stream only with --trace-events.
namespace dq8::host {

class EventLog {
  public:
    // `target`: nullopt (no stream), "-" (stdout) or a file. Throws core::Error when the file cannot be
    // created.
    EventLog(const std::optional<std::string>& target, bool trace_in_stream);
    ~EventLog();
    EventLog(const EventLog&) = delete;
    EventLog& operator=(const EventLog&) = delete;

    bool enabled() const noexcept { return file_ != nullptr || out_ != nullptr; }
    void write(const rt::MachineEvent& event, std::uint64_t vblank, std::uint64_t cycles);
    void flush();

    // The JSON line for one event (no newline).
    static std::string format(const rt::MachineEvent& event, std::uint64_t vblank, std::uint64_t cycles);
    // "trace [cat] msg" for a trace event (no newline).
    static std::string format_trace(const rt::MachineEvent& event, std::uint64_t vblank);

  private:
    std::unique_ptr<std::ofstream> file_;
    std::FILE* out_ = nullptr; // stdout, or null when the file stream is used
    bool trace_in_stream_ = false;
};

} // namespace dq8::host
