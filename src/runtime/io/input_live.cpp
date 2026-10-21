// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/io/input_live.h"

namespace dq8::rt {

void LiveInput::publish(unsigned port, const PadState& state) {
    const std::lock_guard lock(mutex_);
    published_[port & 1u] = state;
}

PadState LiveInput::sample(unsigned port, std::uint64_t vblank) {
    if (!latched_vblank_ || *latched_vblank_ != vblank) {
        const std::lock_guard lock(mutex_);
        latched_ = published_;
        latched_vblank_ = vblank;
    }
    return latched_[port & 1u];
}

} // namespace dq8::rt
