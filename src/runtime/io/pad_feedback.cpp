// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/io/pad_feedback.h"

#include "runtime/platform/platform.h"

namespace dq8::rt {

void LivePadFeedback::actuators(unsigned port, std::uint64_t /*vblank*/, const PadActuators& state) {
    if (port >= ports_.size()) {
        return;
    }
    std::lock_guard lock(mutex_);
    ports_[port].state = state;
    ports_[port].changed = true;
}

void LivePadFeedback::apply(IPlatform& platform) {
    std::array<Port, 2> due{};
    {
        std::lock_guard lock(mutex_);
        due = ports_;
        for (Port& p : ports_) {
            p.changed = false;
        }
    }
    for (unsigned port = 0; port < due.size(); ++port) {
        if (due[port].changed) {
            platform.set_rumble(port, low_frequency(due[port].state), high_frequency(due[port].state));
        }
    }
}

std::uint16_t LivePadFeedback::low_frequency(const PadActuators& state) noexcept {
    return static_cast<std::uint16_t>(state.large * 257u);
}

std::uint16_t LivePadFeedback::high_frequency(const PadActuators& state) noexcept {
    return state.small ? 0xFFFFu : 0u;
}

} // namespace dq8::rt
