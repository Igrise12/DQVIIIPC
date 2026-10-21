// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "host/limiter.h"

#include <thread>

namespace dq8::host {

std::optional<Limiter::Clock::time_point> Limiter::deadline(double virtual_seconds, Clock::time_point now) {
    if (!speed_) {
        return std::nullopt;
    }
    if (!origin_) {
        origin_ = now;
        origin_virtual_ = virtual_seconds;
        return std::nullopt;
    }
    const auto due = *origin_ + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(
                                    (virtual_seconds - origin_virtual_) / *speed_));
    if (now - due > max_lag_) {
        origin_ = now;
        origin_virtual_ = virtual_seconds;
        ++rebases_;
        return std::nullopt;
    }
    if (due <= now) {
        return std::nullopt;
    }
    return due;
}

void Limiter::wait(double virtual_seconds) {
    if (const auto due = deadline(virtual_seconds, Clock::now())) {
        std::this_thread::sleep_until(*due);
    }
}

} // namespace dq8::host
