// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors: the EE timer block of
// ps2xRuntime/src/lib/ps2_memory.cpp (register layout, mode bits, compare/overflow and zero-return rules).
#pragma once

#include "runtime/dma/register_block.h"
#include "runtime/machine/events.h"
#include "runtime/machine/machine_services.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// The four EE timers (0x10000000, 0x10000800, 0x10001000, 0x10001800; COUNT, MODE, COMP and, for timers 0
// and 1, HOLD). Counts are not stepped: each timer remembers its count at a base time and derives the
// current count from the virtual clock (bus clock / 1, / 16, / 256, or the hblank clock from
// VideoTiming::line_start). Compare and overflow are events at the exact cycle the count reaches them;
// whichever of the event or a guest read sees the crossing first sets the flag and raises the INTC line
// (timers 0-3 are INTC 9-12) on the flag's 0 -> 1 transition.
namespace dq8::rt {

class EeTimers final : public IDevice, public IRegisterBlock {
  public:
    static constexpr std::uint32_t base = 0x10000000u;
    static constexpr std::uint32_t size = 0x1840u;
    static constexpr std::uint32_t timer_stride = 0x800u;
    static constexpr std::uint32_t reg_count = 0x00u;
    static constexpr std::uint32_t reg_mode = 0x10u;
    static constexpr std::uint32_t reg_comp = 0x20u;
    static constexpr std::uint32_t reg_hold = 0x30u;

    // MODE bits.
    static constexpr std::uint32_t mode_clks = 0x3u; // 0 bus, 1 bus/16, 2 bus/256, 3 hblank
    static constexpr std::uint32_t mode_gate = 1u << 2;
    static constexpr std::uint32_t mode_zret = 1u << 6;
    static constexpr std::uint32_t mode_cue = 1u << 7;
    static constexpr std::uint32_t mode_cmpe = 1u << 8;
    static constexpr std::uint32_t mode_ovfe = 1u << 9;
    static constexpr std::uint32_t mode_equf = 1u << 10;
    static constexpr std::uint32_t mode_ovff = 1u << 11;

    std::string_view name() const override { return "ee-timers"; }
    void attach(MachineServices& machine) override;
    void reset() override;

    bool read32(std::uint32_t offset, std::uint32_t& value, const MmioContext& at) override;
    bool write32(std::uint32_t offset, std::uint32_t value, std::uint32_t mask,
                 const MmioContext& at) override;

    // Host-side views for tests and diagnostics (they bring the timer up to date first).
    std::uint32_t count(unsigned timer);
    std::uint32_t mode(unsigned timer);

  private:
    struct Timer {
        std::uint32_t count = 0; // at `base_tick`
        std::uint32_t mode = 0;
        std::uint32_t compare = 0;
        std::uint32_t hold = 0;
        std::uint64_t base_tick = 0; // tick position (see tick_position) the count was taken at
        EventId event = 0;
    };

    // Ticks of the timer's clock that have occurred by cycle t: floor(t / divider), or the number of
    // scan lines started after line 0 for the hblank clock.
    std::uint64_t tick_position(const Timer& t, Cycles at) const;
    // The first cycle at which the tick position reaches `position`.
    Cycles cycle_of_tick(const Timer& t, std::uint64_t position) const;
    // Folds the ticks since base_tick into the count, sets flags and raises interrupts for crossings.
    void update(unsigned index);
    // Re-arms the next compare/overflow event of a timer.
    void schedule(unsigned index);
    void rebase(unsigned index);

    MachineServices* machine_ = nullptr;
    std::optional<RegisterWindow> window_;
    std::array<Timer, 4> timers_{};
};

} // namespace dq8::rt
