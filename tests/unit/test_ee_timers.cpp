// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// EE timers (session B "Done when": timer compare on all four clock sources). Counting is derived from the
// virtual clock, so a compare interrupt is an event at an exact cycle, with or without guest reads.
#include "runtime/ee/guest_stop.h"
#include "runtime/machine/interrupts.h"
#include "support/ee_rig.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

using namespace dq8::rt;
using dq8::test::EeRig;

namespace {

constexpr std::uint32_t timer_base(unsigned n) {
    return 0x10000000u + n * 0x800u;
}
constexpr std::uint32_t count = 0x00u, mode = 0x10u, comp = 0x20u;
constexpr std::uint32_t cue = 1u << 7, cmpe = 1u << 8, ovfe = 1u << 9, zret = 1u << 6, equf = 1u << 10,
                        ovff = 1u << 11;

std::uint32_t intc_bit(unsigned timer) {
    return 1u << (static_cast<unsigned>(IntcLine::timer0) + timer);
}

// A rig whose MMIO accesses cost no virtual time, so a test can name exact cycles.
struct FreeRig : EeRig {
    FreeRig() { mem().set_mmio_bus(&machine.mmio(), &machine.clock(), 0); }
};

} // namespace

TEST_CASE("ee timers: compare fires at the exact cycle on every clock source", "[timers]") {
    for (unsigned timer = 0; timer < 4; ++timer) {
        for (std::uint32_t clks = 0; clks < 4; ++clks) {
            INFO("timer " << timer << " clock source " << clks);
            FreeRig rig;
            rig.machine.advance(1000); // an arbitrary start, not on a tick boundary
            const Cycles t0 = rig.machine.clock().now();
            rig.w32(timer_base(timer) + comp, 5);
            rig.w32(timer_base(timer) + mode, clks | cue | cmpe);
            rig.w32(timer_base(timer) + count, 0);
            Cycles expected = 0;
            if (clks < 3) {
                const Cycles divider = clks == 0 ? 2 : clks == 1 ? 32 : 512;
                expected = (t0 / divider + 5) * divider;
            } else {
                const VideoTiming& video = rig.machine.video();
                std::uint64_t line = 0;
                while (video.line_start(line + 1) <= t0) {
                    ++line;
                }
                expected = video.line_start(line + 5);
            }
            REQUIRE(rig.machine.events().next_time() == expected);
            rig.machine.advance(expected - 1);
            CHECK((rig.machine.interrupts().intc_stat() & intc_bit(timer)) == 0u);
            CHECK(rig.r32(timer_base(timer) + count) == 4u);
            rig.machine.advance(expected);
            CHECK((rig.machine.interrupts().intc_stat() & intc_bit(timer)) != 0u);
            CHECK((rig.r32(timer_base(timer) + mode) & equf) != 0u);
            CHECK(rig.r32(timer_base(timer) + count) == 5u);
        }
    }
}

TEST_CASE("ee timers: overflow, the one-shot flag edge, and zero-return periods", "[timers]") {
    FreeRig rig;
    InterruptController& intc = rig.machine.interrupts();
    const std::uint32_t t3 = timer_base(3);
    rig.w32(t3 + mode, 2u | cue | ovfe);
    rig.w32(t3 + count, 0xFFF0u);
    rig.run_for(0xFFFFFFFFu);
    CHECK((intc.intc_stat() & intc_bit(3)) != 0u);
    CHECK(rig.machine.clock().now() == 16u * 512u); // 16 ticks of bus/256 from cycle 0
    // The flag stays set: no further interrupt until it is cleared.
    intc.write_intc_stat(intc_bit(3));
    rig.machine.advance(rig.machine.clock().now() + 0x10000u * 512u);
    CHECK((intc.intc_stat() & intc_bit(3)) == 0u);
    rig.w32(t3 + mode, rig.r32(t3 + mode) | ovff); // clear OVFF: the next overflow interrupts again
    rig.run_for(0xFFFFFFFFu);
    CHECK((intc.intc_stat() & intc_bit(3)) != 0u);

    // ZRET with COMP 10 at bus/16: compare every 10 ticks, the count never reaches COMP.
    FreeRig z;
    const std::uint32_t t1 = timer_base(1);
    z.w32(t1 + comp, 10);
    z.w32(t1 + mode, 1u | cue | cmpe | zret);
    z.w32(t1 + count, 0);
    z.run_for(10u * 32u);
    CHECK((z.machine.interrupts().intc_stat() & intc_bit(1)) != 0u);
    CHECK(z.r32(t1 + count) == 0u);
    z.machine.interrupts().write_intc_stat(intc_bit(1));
    z.w32(t1 + mode, z.r32(t1 + mode) | equf);
    z.machine.advance(z.machine.clock().now() + 5u * 32u);
    CHECK(z.r32(t1 + count) == 5u);
    CHECK((z.machine.interrupts().intc_stat() & intc_bit(1)) == 0u);
    z.machine.advance(z.machine.clock().now() + 5u * 32u);
    CHECK((z.machine.interrupts().intc_stat() & intc_bit(1)) != 0u);
}

TEST_CASE("ee timers: a stopped timer keeps its count; GATE stops the machine", "[timers]") {
    FreeRig rig;
    const std::uint32_t t0 = timer_base(0);
    rig.w32(t0 + mode, 2u | cue);
    rig.machine.advance(10u * 512u);
    CHECK(rig.r32(t0 + count) == 10u);
    rig.w32(t0 + mode, 2u); // CUE off
    rig.machine.advance(50u * 512u);
    CHECK(rig.r32(t0 + count) == 10u);
    CHECK(rig.timers().count(0) == 10u);
    rig.w32(t0 + mode, 2u | cue);
    rig.machine.advance(52u * 512u);
    CHECK(rig.r32(t0 + count) == 12u);
    CHECK_THROWS_AS(rig.w32(t0 + mode, 2u | cue | 4u), GuestStop);
    CHECK(rig.r32(timer_base(2) + 0x30u) == 0u); // no HOLD on timers 2 and 3: unhandled, reads 0
    CHECK(rig.mem().mmio_log().size() == 1u);
}
