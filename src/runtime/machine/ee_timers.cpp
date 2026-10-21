// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors: the EE timer block of
// ps2xRuntime/src/lib/ps2_memory.cpp (advanceEeTimers, cyclesUntilNextEeTimerInterrupt, the timer cases of
// writeIORegister), reworked to count lazily from the virtual clock with compares and overflows as events.
#include "runtime/machine/ee_timers.h"

#include "runtime/ee/guest_stop.h"
#include "runtime/machine/interrupts.h"
#include "runtime/machine/mmio.h"

#include <limits>
#include <string>
#include <utility>

namespace dq8::rt {

namespace {

// EE cycles per tick of the clock sources 0-2 (the bus clock is half the EE clock).
constexpr std::array<Cycles, 3> divider = {2, 32, 512};
constexpr std::uint64_t no_event = std::numeric_limits<std::uint64_t>::max();

struct Step {
    std::uint32_t count = 0;
    bool compare = false;
    bool overflow = false;
};

// The count after `ticks` ticks from `count`, and whether it passed the compare value or wrapped. With
// ZRET the count returns to 0 on the tick that would make it equal COMP (period COMP), as PCSX2 and
// upstream do; COMP = 0 counts freely.
Step advance(std::uint32_t count, std::uint32_t compare, bool zret, std::uint64_t ticks) {
    Step s;
    s.count = count;
    if (ticks == 0) {
        return s;
    }
    if (!zret || compare == 0) {
        const std::uint64_t to_compare =
            ((compare - count) & 0xFFFFu) == 0 ? 0x10000u : ((compare - count) & 0xFFFFu);
        const std::uint64_t to_overflow = 0x10000u - count;
        s.compare = ticks >= to_compare;
        s.overflow = ticks >= to_overflow;
        s.count = static_cast<std::uint32_t>((count + ticks) & 0xFFFFu);
        return s;
    }
    std::uint64_t rest = ticks;
    std::uint32_t c = count;
    if (c >= compare) {
        const std::uint64_t to_overflow = 0x10000u - c;
        if (rest < to_overflow) {
            s.count = static_cast<std::uint32_t>(c + rest);
            return s;
        }
        s.overflow = true;
        rest -= to_overflow;
        c = 0;
    }
    const std::uint64_t to_compare = compare - c;
    if (rest < to_compare) {
        s.count = static_cast<std::uint32_t>(c + rest);
        return s;
    }
    s.compare = true;
    s.count = static_cast<std::uint32_t>((rest - to_compare) % compare);
    return s;
}

// Ticks until the next compare (when wanted) or overflow (when wanted) from `count`; no_event if neither.
std::uint64_t ticks_to_event(std::uint32_t count, std::uint32_t compare, bool zret, bool want_compare,
                             bool want_overflow) {
    std::uint64_t best = no_event;
    if (!zret || compare == 0) {
        if (want_compare) {
            const std::uint32_t d = (compare - count) & 0xFFFFu;
            best = d == 0 ? 0x10000u : d;
        }
        if (want_overflow && 0x10000u - count < best) {
            best = 0x10000u - count;
        }
        return best;
    }
    if (count >= compare) {
        const std::uint64_t to_overflow = 0x10000u - count;
        if (want_overflow) {
            best = to_overflow;
        }
        if (want_compare && to_overflow + compare < best) {
            best = to_overflow + compare;
        }
        return best;
    }
    if (want_compare) {
        best = compare - count;
    }
    return best; // the count resets at COMP before it can overflow
}

} // namespace

void EeTimers::attach(MachineServices& machine) {
    machine_ = &machine;
    window_.emplace(*this, 0u);
    machine.mmio().map(base, size, *window_);
}

void EeTimers::reset() {
    for (Timer& t : timers_) {
        if (t.event != 0) {
            machine_->events().cancel(t.event);
        }
        t = Timer{};
    }
}

std::uint64_t EeTimers::tick_position(const Timer& t, Cycles at) const {
    const std::uint32_t clks = t.mode & mode_clks;
    if (clks < 3) {
        return at / divider[clks];
    }
    // hblank clock: the number of scan lines that have started after line 0. Estimate in floating point,
    // then settle exactly against line_start so the result is the same on every host.
    const VideoTiming& video = machine_->video();
    const double per_line = static_cast<double>(video.line_start(1'000'000)) / 1'000'000.0;
    auto n = static_cast<std::uint64_t>(static_cast<double>(at) / per_line);
    while (n > 0 && video.line_start(n) > at) {
        --n;
    }
    while (video.line_start(n + 1) <= at) {
        ++n;
    }
    return n;
}

Cycles EeTimers::cycle_of_tick(const Timer& t, std::uint64_t position) const {
    const std::uint32_t clks = t.mode & mode_clks;
    if (clks < 3) {
        return position * divider[clks];
    }
    return machine_->video().line_start(position);
}

void EeTimers::update(unsigned index) {
    Timer& t = timers_[index];
    const std::uint64_t now = tick_position(t, machine_->clock().now());
    if ((t.mode & mode_cue) == 0 || now <= t.base_tick) {
        t.base_tick = now;
        return;
    }
    const Step s = advance(t.count, t.compare, (t.mode & mode_zret) != 0, now - t.base_tick);
    t.count = s.count;
    t.base_tick = now;
    bool raise = false;
    if (s.compare && (t.mode & mode_cmpe) != 0 && (t.mode & mode_equf) == 0) {
        t.mode |= mode_equf;
        raise = true;
    }
    if (s.overflow && (t.mode & mode_ovfe) != 0 && (t.mode & mode_ovff) == 0) {
        t.mode |= mode_ovff;
        raise = true;
    }
    if (raise) {
        machine_->interrupts().raise_intc(
            static_cast<IntcLine>(static_cast<unsigned>(IntcLine::timer0) + index));
        machine_->note_activity();
    }
}

void EeTimers::schedule(unsigned index) {
    Timer& t = timers_[index];
    if (t.event != 0) {
        machine_->events().cancel(t.event);
        t.event = 0;
    }
    if ((t.mode & mode_cue) == 0) {
        return;
    }
    const bool want_compare = (t.mode & mode_cmpe) != 0 && (t.mode & mode_equf) == 0;
    const bool want_overflow = (t.mode & mode_ovfe) != 0 && (t.mode & mode_ovff) == 0;
    const std::uint64_t ticks =
        ticks_to_event(t.count, t.compare, (t.mode & mode_zret) != 0, want_compare, want_overflow);
    if (ticks == no_event) {
        return;
    }
    const Cycles at = cycle_of_tick(t, t.base_tick + ticks);
    t.event = machine_->events().schedule(at, "ee-timer", [this, index](Cycles) {
        timers_[index].event = 0;
        update(index);
        schedule(index);
    });
}

void EeTimers::rebase(unsigned index) {
    Timer& t = timers_[index];
    t.base_tick = tick_position(t, machine_->clock().now());
}

std::uint32_t EeTimers::count(unsigned timer) {
    update(timer);
    return timers_[timer].count;
}

std::uint32_t EeTimers::mode(unsigned timer) {
    update(timer);
    return timers_[timer].mode;
}

bool EeTimers::read32(std::uint32_t offset, std::uint32_t& value, const MmioContext& at) {
    const unsigned index = offset / timer_stride;
    const std::uint32_t reg = offset % timer_stride;
    if (index >= timers_.size()) {
        return false;
    }
    Timer& t = timers_[index];
    switch (reg) {
    case reg_count:
        update(index);
        value = t.count;
        break;
    case reg_mode:
        update(index);
        value = t.mode;
        break;
    case reg_comp:
        value = t.compare;
        return true;
    case reg_hold:
        if (index >= 2) {
            return false;
        }
        value = t.hold;
        return true;
    default:
        return false;
    }
    machine_->note_poll(at.pc, base + offset, value);
    return true;
}

bool EeTimers::write32(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, const MmioContext& at) {
    const unsigned index = offset / timer_stride;
    const std::uint32_t reg = offset % timer_stride;
    if (index >= timers_.size()) {
        return false;
    }
    Timer& t = timers_[index];
    switch (reg) {
    case reg_count:
        update(index);
        t.count = merge_masked(t.count, value, mask) & 0xFFFFu;
        rebase(index);
        break;
    case reg_mode: {
        update(index);
        const std::uint32_t written = merge_masked(t.mode & 0x3FFu, value, mask);
        if ((written & mode_gate) != 0) {
            StopInfo info;
            info.reason = StopReason::unsupported;
            info.pc = at.pc;
            info.address = base + offset;
            info.message = "EE timer " + std::to_string(index) + " MODE " + hex32(written) +
                           " enables a gate (MODE.GATE), which the timers do not model";
            machine_->stop(std::move(info));
        }
        // Status bits are cleared by writing 1 (only the bytes written count).
        const std::uint32_t status = (t.mode & (mode_equf | mode_ovff)) & ~(value & mask);
        const bool clock_changed = ((t.mode ^ written) & (mode_clks | mode_cue)) != 0;
        t.mode = (written & 0x3FFu) | status;
        if (clock_changed) {
            rebase(index);
        }
        break;
    }
    case reg_comp:
        update(index);
        t.compare = merge_masked(t.compare, value, mask) & 0xFFFFu;
        break;
    case reg_hold:
        if (index >= 2) {
            return false;
        }
        t.hold = merge_masked(t.hold, value, mask) & 0xFFFFu;
        return true;
    default:
        return false;
    }
    schedule(index);
    machine_->note_activity();
    return true;
}

} // namespace dq8::rt
