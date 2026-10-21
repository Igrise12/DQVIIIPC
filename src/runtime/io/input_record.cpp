// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/io/input_record.h"

#include "runtime/io/input_script.h"

#include <cstdio>
#include <stdexcept>

namespace dq8::rt {

namespace {

std::string axis(const char* name, std::uint8_t value) {
    char buf[16];
    std::snprintf(buf, sizeof buf, " %s=0x%02X", name, static_cast<unsigned>(value));
    return buf;
}

} // namespace

RecordingInput::RecordingInput(IInputSource& inner, const std::filesystem::path& file)
    : inner_(inner), out_(file, std::ios::binary | std::ios::trunc) {
    if (!out_) {
        throw std::runtime_error("cannot create the input recording " + file.string());
    }
    out_ << "dq8in 1\n# Recorded by dq8-game --record-input: one line per change of a port's state.\n";
}

RecordingInput::~RecordingInput() {
    flush();
}

void RecordingInput::flush() {
    out_.flush();
}

std::vector<std::string> RecordingInput::format_changes(unsigned port, std::uint64_t vblank,
                                                        const PadState& state,
                                                        const std::optional<PadState>& previous) {
    const PadState before = previous.value_or(PadState{});
    const std::string at = std::to_string(vblank) + (port == 1 ? " p2 " : " ");
    std::vector<std::string> lines;
    if (state.connected != before.connected) {
        lines.push_back(at + (state.connected ? "connect" : "disconnect"));
    }
    if (!state.connected) {
        return lines;
    }
    // After a reconnect the full state is written, so the replay does not depend on what a script kept.
    const bool changed = !before.connected || state.buttons != before.buttons || state.lx != before.lx ||
                         state.ly != before.ly || state.rx != before.rx || state.ry != before.ry;
    if (changed) {
        std::string line = at + "state " + format_buttons(state.buttons);
        if (state.lx != 0x80) {
            line += axis("lx", state.lx);
        }
        if (state.ly != 0x80) {
            line += axis("ly", state.ly);
        }
        if (state.rx != 0x80) {
            line += axis("rx", state.rx);
        }
        if (state.ry != 0x80) {
            line += axis("ry", state.ry);
        }
        lines.push_back(line);
    }
    return lines;
}

PadState RecordingInput::sample(unsigned port, std::uint64_t vblank) {
    const PadState state = inner_.sample(port, vblank);
    std::optional<PadState>& last = last_[port & 1u];
    for (const std::string& line : format_changes(port & 1u, vblank, state, last)) {
        out_ << line << '\n';
        ++lines_;
    }
    last = state;
    return state;
}

} // namespace dq8::rt
