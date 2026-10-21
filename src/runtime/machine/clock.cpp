// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/machine/clock.h"

namespace dq8::rt {

namespace {

// floor(n * mul / div) without overflowing 64 bits for any n a run can reach: mul / div is split into a
// whole and a fractional part, and n * remainder stays below 2^64 while n < 2^64 / div.
Cycles scaled(std::uint64_t n, std::uint64_t mul, std::uint64_t div) noexcept {
    const std::uint64_t whole = mul / div;
    const std::uint64_t rest = mul % div;
    return n * whole + (n * rest) / div;
}

} // namespace

Cycles VideoTiming::field_start(std::uint64_t n) const noexcept {
    return scaled(n, ee_clock_hz * field_rate_den, field_rate_num);
}

Cycles VideoTiming::line_start(std::uint64_t n) const noexcept {
    return scaled(n, ee_clock_hz * field_rate_den * 2, field_rate_num * lines_per_frame);
}

Cycles VideoTiming::vblank_end(std::uint64_t n) const noexcept {
    // Lines counted from the field's own start: field n begins at line n * lines_per_frame / 2, which is a
    // half line for odd fields in NTSC; add the offset in cycles instead of rounding the line number.
    return field_start(n) +
           scaled(vblank_lines, ee_clock_hz * field_rate_den * 2, field_rate_num * lines_per_frame);
}

std::uint64_t VideoTiming::fields_started(Cycles t) const noexcept {
    // Smallest n with field_start(n) > t: estimate, then correct by at most a step either way.
    const std::uint64_t per_field = ee_clock_hz * field_rate_den / field_rate_num;
    std::uint64_t n = per_field == 0 ? 0 : t / per_field;
    while (n > 0 && field_start(n) > t) {
        --n;
    }
    while (field_start(n) <= t) {
        ++n;
    }
    return n;
}

} // namespace dq8::rt
