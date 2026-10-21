// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "game/mode_watcher.h"

#include "runtime/ee/guest_memory.h"
#include "runtime/ee/guest_stop.h"

#include <array>
#include <stdexcept>

namespace dq8::game {

ModeWatcher::ModeWatcher(std::uint32_t mode_variable) : address_(mode_variable) {
    if (mode_variable == 0 || (mode_variable & 3u) != 0) {
        throw std::invalid_argument("mode variable " + rt::hex32(mode_variable) +
                                    " is not a 4-aligned guest address");
    }
}

void ModeWatcher::attach(rt::MachineServices& machine) {
    machine_ = &machine;
    machine.add_vblank_listener(rt::VblankPhase::intc, *this);
}

void ModeWatcher::reset() {
    mode_ = 0;
    changes_ = 0;
}

void ModeWatcher::on_vblank_start(std::uint64_t /*field*/) {
    std::array<std::uint8_t, 4> b{};
    machine_->memory().read_bytes(address_, b);
    const std::uint32_t value = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
                                (static_cast<std::uint32_t>(b[2]) << 16) |
                                (static_cast<std::uint32_t>(b[3]) << 24);
    if (value == mode_) {
        return;
    }
    const std::uint32_t previous = mode_;
    mode_ = value;
    ++changes_;
    machine_->emit(rt::MachineEvent{"mode", {}}
                       .add("mode", std::int64_t{static_cast<std::int32_t>(value)})
                       .add("previous", std::int64_t{static_cast<std::int32_t>(previous)}));
}

} // namespace dq8::game
