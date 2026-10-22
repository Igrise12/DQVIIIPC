// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xIOP/src/emulator/imports/iop_timrman.cpp (the timer table, the id encoding, the argument checks).
// Changes: compare events on the machine's EventQueue and handlers in the interrupt context instead of a
// polled budget.
#include "runtime/iop/iop_exec.h"
#include "runtime/iop/kernel/hle.h"
#include "runtime/machine/machine_services.h"

#include <algorithm>
#include <array>

// timrman as pure HLE (Docs/milestones/M4-playable.md, session A): the six IOP hardware timers are handed out
// and configured as on the IOP, but no counter register exists. A started timer with a handler raises a
// compare event after `compare` counts of its source; the handler runs in the interrupt context with its
// argument and returns the next compare value (0 stops the timer). The counter restarts at each compare.
namespace dq8::rt {

namespace {

constexpr std::array<std::size_t, 6> allocation_order{2, 5, 4, 3, 0, 1};
constexpr std::array<std::uint32_t, 6> addresses{0xBF801100u, 0xBF801110u, 0xBF801120u,
                                                 0xBF801480u, 0xBF801490u, 0xBF8014A0u};
constexpr std::array<std::uint32_t, 6> sources{0x0B, 0x0D, 0x01, 0x05, 0x01, 0x01};
constexpr std::array<std::uint32_t, 6> widths{16, 16, 16, 32, 32, 32};
constexpr std::array<std::uint32_t, 6> max_prescales{1, 1, 8, 1, 256, 256};
// Timer sources: the system clock, the pixel clock, the horizontal blank.
constexpr std::uint32_t source_pixel = 2;
constexpr std::uint32_t source_hline = 4;
constexpr std::uint64_t pixel_clock_hz = 13'500'000;

std::uint32_t timer_id(std::size_t index) {
    return (static_cast<std::uint32_t>(index + 1) << 28) | (addresses[index] >> 4);
}

// The timer an id names, when it is allocated.
std::optional<std::size_t> timer_index(HleCall& c, std::uint32_t id) {
    const std::uint32_t n = id >> 28;
    if (n == 0 || n > 6) {
        return std::nullopt;
    }
    const std::size_t index = n - 1;
    if ((id & 0x0FFFFFFFu) != (addresses[index] >> 4) || c.exec().timers()[index].users == 0) {
        return std::nullopt;
    }
    return index;
}

std::uint32_t mask(std::size_t index, std::uint32_t value) {
    return widths[index] == 16 ? (value & 0xFFFFu) : value;
}

void alloc_hard_timer(HleCall& c) {
    const std::uint32_t source = c.arg(0);
    const std::uint32_t size = c.arg(1);
    const std::uint32_t prescale = c.arg(2);
    auto& timers = c.exec().timers();
    for (const std::size_t index : allocation_order) {
        IopHardTimer& t = timers[index];
        if (t.users != 0 || (sources[index] & source) == 0 || widths[index] != size ||
            max_prescales[index] < prescale) {
            continue;
        }
        t = IopHardTimer{};
        t.users = 1;
        t.source = source;
        t.prescale = std::max<std::uint32_t>(prescale, 1);
        c.ret(timer_id(index));
        return;
    }
    c.ret(iop_error::no_timer);
}

void free_hard_timer(HleCall& c) {
    const auto index = timer_index(c, c.arg(0));
    if (!index) {
        c.ret(iop_error::illegal_timerid);
        return;
    }
    IopHardTimer& t = c.exec().timers()[*index];
    if (--t.users == 0) {
        c.exec().cancel_timer(*index);
        t = IopHardTimer{};
    }
    c.ret(0);
}

void set_timer_handler(HleCall& c) {
    const auto index = timer_index(c, c.arg(0));
    if (!index) {
        c.ret(iop_error::illegal_timerid);
        return;
    }
    IopHardTimer& t = c.exec().timers()[*index];
    if (t.running) {
        c.ret(iop_error::timer_busy);
        return;
    }
    t.compare = mask(*index, c.arg(1));
    t.handler = c.arg(2);
    t.common = c.arg(3);
    t.gp = c.caller_gp();
    c.ret(0);
}

void setup_hard_timer(HleCall& c) {
    const auto index = timer_index(c, c.arg(0));
    if (!index) {
        c.ret(iop_error::illegal_timerid);
        return;
    }
    IopHardTimer& t = c.exec().timers()[*index];
    const std::uint32_t source = c.arg(1);
    const std::uint32_t mode = c.arg(2);
    const std::uint32_t prescale = c.arg(3);
    if (t.running) {
        c.ret(iop_error::timer_busy);
        return;
    }
    if (mode != 0 && mode != 1 && mode != 3 && mode != 5 && mode != 7) {
        c.ret(iop_error::illegal_mode);
        return;
    }
    if ((sources[*index] & source) == 0) {
        c.ret(iop_error::illegal_source);
        return;
    }
    if (prescale == 0 || prescale > max_prescales[*index]) {
        c.ret(iop_error::illegal_prescale);
        return;
    }
    t.source = source;
    t.mode = mode;
    t.prescale = prescale;
    t.configured = true;
    c.ret(0);
}

void start_hard_timer(HleCall& c) {
    const auto index = timer_index(c, c.arg(0));
    if (!index) {
        c.ret(iop_error::illegal_timerid);
        return;
    }
    IopHardTimer& t = c.exec().timers()[*index];
    if (t.running) {
        c.ret(iop_error::timer_busy);
        return;
    }
    if (!t.configured) {
        c.ret(iop_error::timer_not_setup);
        return;
    }
    t.running = true;
    t.started = c.exec().now();
    c.exec().schedule_timer(*index, t.started);
    c.ret(0);
}

void stop_hard_timer(HleCall& c) {
    const auto index = timer_index(c, c.arg(0));
    if (!index) {
        c.ret(iop_error::illegal_timerid);
        return;
    }
    IopHardTimer& t = c.exec().timers()[*index];
    if (!t.running) {
        c.ret(iop_error::timer_not_inuse);
        return;
    }
    c.exec().cancel_timer(*index);
    t.running = false;
    c.ret(0);
}

constexpr HleFunction timrman_table[] = {
    {"timrman", 4, "AllocHardTimer", alloc_hard_timer},
    {"timrman", 6, "FreeHardTimer", free_hard_timer},
    {"timrman", 20, "SetTimerHandler", set_timer_handler},
    {"timrman", 22, "SetupHardTimer", setup_hard_timer},
    {"timrman", 23, "StartHardTimer", start_hard_timer},
    {"timrman", 24, "StopHardTimer", stop_hard_timer},
};

} // namespace

std::span<const HleFunction> hle_timrman() {
    return timrman_table;
}

Cycles IopExec::timer_cycles(const IopHardTimer& t, std::uint64_t ticks) const noexcept {
    const std::uint64_t counts = ticks * std::max<std::uint32_t>(t.prescale, 1);
    if ((t.source & source_pixel) != 0) {
        return std::max<Cycles>(1, (counts * ee_clock_hz + pixel_clock_hz - 1) / pixel_clock_hz);
    }
    if ((t.source & source_hline) != 0) {
        // One line is ee_clock_hz * den * 2 / (num * lines_per_frame) cycles (clock.h).
        const VideoTiming& v = machine_.video();
        const std::uint64_t num = v.field_rate_num * v.lines_per_frame;
        return std::max<Cycles>(1, (counts * ee_clock_hz * v.field_rate_den * 2 + num - 1) / num);
    }
    return std::max<Cycles>(1, counts * ee_cycles_per_iop_cycle);
}

void IopExec::schedule_timer(std::size_t index, Cycles from) {
    IopHardTimer& t = timers_.at(index);
    cancel_timer(index);
    if (!t.running || t.handler == 0) {
        return;
    }
    const std::uint32_t compare = mask(index, t.compare);
    const std::uint64_t ticks = compare != 0 ? compare : (std::uint64_t{1} << widths[index]);
    const Cycles when = std::max(from + timer_cycles(t, ticks), now_);
    t.event =
        machine_.events().schedule(when, "iop_timer", [this, index](Cycles at) { timer_due(index, at); });
}

void IopExec::cancel_timer(std::size_t index) {
    IopHardTimer& t = timers_.at(index);
    if (t.event != 0) {
        machine_.events().cancel(t.event);
        t.event = 0;
    }
}

void IopExec::timer_due(std::size_t index, Cycles when) {
    IopHardTimer& t = timers_.at(index);
    t.event = 0;
    if (!t.running) {
        return;
    }
    t.started = when;
    queue_irq(IopIrqJob{IopIrqJob::Kind::timer, t.handler, t.common, t.gp, static_cast<std::uint32_t>(index)},
              when);
}

} // namespace dq8::rt
