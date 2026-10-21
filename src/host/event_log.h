// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "host/options.h"
#include "runtime/machine/machine_services.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

// dq8-game's JSON-lines event stream (architecture §11.1, §16; ADR-0007): one object per line,
// {"ev": type, "vblank": N, "cycles": C, ...fields}. Only deterministic fields go in (no wall time, no host
// pointers), so two runs of the same disc, build and input give byte-identical streams. Written on the
// machine thread only. --trace echoes the events of the chosen categories on stderr as one-line text.
namespace dq8::host {

class EventLog {
  public:
    // `target`: nullopt (no stream), "-" (stdout) or a file. Throws core::Error when the file cannot be
    // created.
    EventLog(const std::optional<std::string>& target, std::uint32_t trace_mask);
    ~EventLog();
    EventLog(const EventLog&) = delete;
    EventLog& operator=(const EventLog&) = delete;

    bool enabled() const noexcept { return out_ != nullptr || trace_ != 0; }
    void write(const rt::MachineEvent& event, std::uint64_t vblank, std::uint64_t cycles);
    void flush();

    // The JSON line for one event (no newline).
    static std::string format(const rt::MachineEvent& event, std::uint64_t vblank, std::uint64_t cycles);
    // The --trace category an event type belongs to (a TraceBit), or 0.
    static std::uint32_t category(std::string_view type) noexcept;

  private:
    std::unique_ptr<std::ofstream> file_;
    std::FILE* out_ = nullptr; // stdout, or null when the file stream is used
    std::uint32_t trace_ = 0;
};

} // namespace dq8::host
