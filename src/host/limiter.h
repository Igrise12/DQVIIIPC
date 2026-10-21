// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// The limiter (architecture §12.4): between machine slices it waits until wall time catches up with virtual
// time at the chosen speed. Wall time only ever decides how long the host sleeps; it never reaches the guest.
// When the host falls behind by more than `max_lag`, it re-bases instead of running fast to catch up (the
// policy is to present fewer frames, never to change guest timing).
namespace dq8::host {

class Limiter {
  public:
    using Clock = std::chrono::steady_clock;

    // `speed`: 1.0 = real time; nullopt = no limit.
    explicit Limiter(std::optional<double> speed,
                     std::chrono::milliseconds max_lag = std::chrono::milliseconds(250)) noexcept
        : speed_(speed), max_lag_(max_lag) {}

    // The wall-clock moment virtual time `virtual_seconds` is due, re-basing when far behind.
    // Returns nullopt when unlimited (or already due).
    std::optional<Clock::time_point> deadline(double virtual_seconds, Clock::time_point now);
    // Sleeps until deadline(virtual_seconds, now()).
    void wait(double virtual_seconds);

    bool unlimited() const noexcept { return !speed_; }
    std::uint64_t rebases() const noexcept { return rebases_; }

  private:
    std::optional<double> speed_;
    std::chrono::milliseconds max_lag_;
    std::optional<Clock::time_point> origin_; // wall time of virtual time `origin_virtual_`
    double origin_virtual_ = 0;
    std::uint64_t rebases_ = 0;
};

} // namespace dq8::host
