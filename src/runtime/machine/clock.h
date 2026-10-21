// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>

// Virtual time (architecture §12.4, ADR-0007). Everything the guest can observe about time (vblanks, timers,
// alarms, DMA, RPC and disc completions, COP0 Count) is derived from one counter of EE cycles that only guest
// work and idle fast-forward advance. Wall-clock time never reaches it: the limiter waits between machine
// slices, outside guest-visible state.
namespace dq8::rt {

using Cycles = std::uint64_t;

// The EE core clock. Bus-clock devices (timers, DMA) run at half of it.
inline constexpr Cycles ee_clock_hz = 294'912'000;
inline constexpr Cycles bus_clock_hz = ee_clock_hz / 2;

// Converts a duration to cycles, rounding down. Exact for whole microseconds and nanoseconds.
constexpr Cycles cycles_from_us(std::uint64_t us) noexcept {
    return us * (ee_clock_hz / 1'000'000) + us * (ee_clock_hz % 1'000'000) / 1'000'000;
}
// Cycles to move `bytes` at `bytes_per_second`, rounding up (at least 1 for a non-empty transfer).
constexpr Cycles cycles_for_bytes(std::uint64_t bytes, std::uint64_t bytes_per_second) noexcept {
    return bytes == 0 ? 0 : (bytes * ee_clock_hz + bytes_per_second - 1) / bytes_per_second;
}

class VirtualClock {
  public:
    Cycles now() const noexcept { return now_; }
    // Guest work: called by the safe points with the cost model's charges.
    void charge(Cycles cycles) noexcept { now_ += cycles; }
    // Jumps forward to `t` (idle fast-forward, spin skipping); never moves backwards.
    void advance_to(Cycles t) noexcept {
        if (t > now_) {
            now_ = t;
        }
    }

    // The scheduler sets the deadline to the next event; a safe point whose clock is due yields to it.
    Cycles deadline() const noexcept { return deadline_; }
    void set_deadline(Cycles t) noexcept { deadline_ = t; }
    bool due() const noexcept { return now_ >= deadline_; }
    // Makes the next safe point yield (an interrupt was raised, a reschedule was requested).
    void force_safe_point() noexcept { deadline_ = now_; }

    void reset() noexcept {
        now_ = 0;
        deadline_ = 0;
    }

  private:
    Cycles now_ = 0;
    Cycles deadline_ = 0;
};

// Video timing for the vblank and hblank events, computed exactly from rational rates so that field n starts
// at the same cycle on every host and every run. NTSC: 60000/1001 fields per second, 525 lines per frame
// (262.5 per field), so one field is 4,920,115.2 cycles and one line about 18,743.3.
struct VideoTiming {
    std::uint64_t field_rate_num = 60'000; // fields per second = num / den
    std::uint64_t field_rate_den = 1'001;
    std::uint32_t lines_per_frame = 525; // two fields
    // Lines from vblank start to vblank end. Inferred from the PS2's GS timing tables; tuned during bring-up
    // against PCSX2 (Docs/milestones/M3-boots.md).
    std::uint32_t vblank_lines = 22;

    static VideoTiming ntsc() noexcept { return {}; }
    static VideoTiming pal() noexcept { return {50, 1, 625, 25}; }

    // First cycle of field n (vblank start of field n), floor(n * ee_clock_hz * den / num).
    Cycles field_start(std::uint64_t n) const noexcept;
    // First cycle of scan line n counted from cycle 0 (hblank clock for the timers),
    // floor(n * ee_clock_hz * den * 2 / (num * lines_per_frame)).
    Cycles line_start(std::uint64_t n) const noexcept;
    // Vblank end of field n: vblank_lines lines after field_start(n).
    Cycles vblank_end(std::uint64_t n) const noexcept;
    // The number of whole fields that have started by cycle t (the field index t lies in, plus one).
    std::uint64_t fields_started(Cycles t) const noexcept;
};

} // namespace dq8::rt
