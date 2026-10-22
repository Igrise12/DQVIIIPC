// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/machine/clock.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The console's real-time clock in virtual time (ADR-0016). The guest never sees the host clock: the RTC is
// an epoch (the console clock at boot, virtual time 0) plus the virtual time elapsed, in whole seconds. The
// epoch is a fixed date by default, a date given with --rtc, or the host's local time at start with --rtc
// host (then recorded in the run's start event). The memory card stamps entries with it.
namespace dq8::rt {

// A calendar date and time as the PS2 keeps it, years 2000 to 2099.
struct RtcDateTime {
    std::uint16_t year = 2000;
    std::uint8_t month = 1; // 1..12
    std::uint8_t day = 1;   // 1..31
    std::uint8_t hour = 0;
    std::uint8_t minute = 0;
    std::uint8_t second = 0;
    friend bool operator==(const RtcDateTime&, const RtcDateTime&) = default;
};

// "YYYY-MM-DDTHH:MM:SS".
std::string format_rtc(const RtcDateTime& t);
// Parses format_rtc's form; nullopt for anything else, including dates that do not exist (2005-02-30) and
// years outside 2000..2099.
std::optional<RtcDateTime> parse_rtc(std::string_view text);
// Seconds since 2000-01-01T00:00:00 and back.
std::int64_t rtc_seconds(const RtcDateTime& t) noexcept;
RtcDateTime rtc_from_seconds(std::int64_t seconds) noexcept;

class VirtualRtc {
  public:
    // The default epoch: 2005-11-15T12:00:00.
    static RtcDateTime default_epoch() noexcept;
    // The host's local time now, clamped to 2000..2099 (for --rtc host).
    static RtcDateTime host_now();

    explicit VirtualRtc(RtcDateTime epoch = default_epoch()) noexcept : epoch_(epoch) {}

    const RtcDateTime& epoch() const noexcept { return epoch_; }
    // The console clock at virtual time `t`: the epoch plus whole seconds elapsed (rounding down).
    RtcDateTime at(Cycles t) const noexcept;

  private:
    RtcDateTime epoch_;
};

} // namespace dq8::rt
