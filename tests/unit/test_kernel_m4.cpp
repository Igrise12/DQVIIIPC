// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// M4 session D's kernel and stop-message work: SetMemoryMode as a documented noop, ResetEE accepted while
// the units it names are idle, break and trap stops that decode the instruction and name its unit and
// function (and flag code straight-line flow cannot reach), and miss messages for jumps that leave their
// function.
#include "runtime/ee/guest_stop.h"
#include "runtime/kernel/kernel_services.h"
#include "runtime/machine/mmio.h"
#include "support/machine_harness.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace {

using namespace dq8::rt;
using dq8::test::guest_return;
using dq8::test::HostProgram;
using dq8::test::poke;
using dq8::test::TestMachine;

constexpr std::uint32_t k_main = HostProgram::base;
constexpr std::uint32_t k_other = HostProgram::base + 0x40;

constexpr std::uint32_t nop = 0x00000000u;
constexpr std::uint32_t jr_ra = 0x03E00008u;
constexpr std::uint32_t break7 = (7u << 6) | 0x0Du;
constexpr std::uint32_t break6 = (6u << 6) | 0x0Du;
constexpr std::uint32_t teq_a0_zero_7 = (4u << 21) | (0u << 16) | (7u << 6) | 0x34u;

StopInfo stop_of(const SliceResult& r) {
    REQUIRE(r.stopped);
    return *r.stop;
}

// A DMA channel register block whose CHCR reads with STR set: a transfer in flight.
class BusyChannel final : public IMmioDevice {
  public:
    std::string_view name() const override { return "busy channel"; }
    bool read(std::uint32_t /*offset*/, std::uint32_t width, std::uint8_t* out, const MmioContext&) override {
        const std::uint32_t chcr = 1u << 8;
        std::memcpy(out, &chcr, width < 4 ? width : 4);
        return true;
    }
    bool write(std::uint32_t, std::uint32_t, const std::uint8_t*, const MmioContext&) override {
        return true;
    }
};

// The words the "function" at k_main holds, written by the guest code itself (pc stays at the entry, as
// generated code leaves it before a break or a trap).
std::uint32_t g_words[8];
bool g_trap = false;

void run_site(std::initializer_list<std::uint32_t> words, bool trap, StopInfo& out) {
    std::memset(g_words, 0, sizeof g_words);
    std::size_t i = 0;
    for (const std::uint32_t w : words) {
        g_words[i++] = w;
    }
    g_trap = trap;
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        for (std::uint32_t k = 0; k < 8; ++k) {
            poke(rdram, k_main + 4 * k, g_words[k]);
        }
        ctx->pc = k_main;
        if (g_trap) {
            rt->handleTrap(rdram, ctx);
        } else {
            rt->handleBreak(rdram, ctx);
        }
        guest_return(ctx);
    });
    p.set(k_other, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) { guest_return(ctx); });
    TestMachine m(p.table());
    out = stop_of(m.run_from(k_main));
}

} // namespace

TEST_CASE("kernel M4: SetMemoryMode is a documented noop", "[kernel][m4]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) { guest_return(ctx); });
    {
        TestMachine m(p.table());
        m.boot(k_main);
        CHECK(m.call(0x85, {0}) == 0);
        const KernelServiceInfo* info = m.machine->kernel().find("SetMemoryMode");
        REQUIRE(info != nullptr);
        CHECK((info->flags & kernel_service_noop) != 0);
        CHECK_FALSE(info->note.empty());
    }
    MachineConfig strict;
    strict.strict_kernel = true;
    TestMachine m(p.table(), strict);
    m.boot(k_main);
    CHECK_THROWS_AS(m.call(0x85, {0}), GuestStop);
}

TEST_CASE("kernel M4: ResetEE is accepted while the units it names are idle", "[kernel][m4]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) { guest_return(ctx); });
    {
        TestMachine m(p.table());
        m.boot(k_main);
        CHECK(m.call(0x01, {0x7F}) == 0); // every unit, all idle
        CHECK(m.call(0x01, {0x00}) == 0);
    }
    {
        TestMachine m(p.table());
        m.boot(k_main);
        try {
            m.call(0x01, {0x80});
            FAIL("an unknown unit bit was accepted");
        } catch (const GuestStop& stop) {
            CHECK(stop.info().reason == StopReason::unsupported);
            CHECK(stop.info().message.find("units this kernel does not know") != std::string::npos);
        }
    }
    BusyChannel busy;
    TestMachine m(p.table());
    m.machine->mmio().map(0x10009000u, 0x100, busy); // VIF1's channel: a transfer in flight
    m.boot(k_main);
    CHECK(m.call(0x01, {0x08}) == 0); // the GIF alone is idle
    try {
        m.call(0x01, {0x04}); // VIF1
        FAIL("ResetEE of a busy unit was accepted");
    } catch (const GuestStop& stop) {
        CHECK(stop.info().reason == StopReason::unsupported);
        CHECK(stop.info().message.find("VIF1 is busy (DMA channel 1 running)") != std::string::npos);
    }
}

TEST_CASE("kernel M4: break and trap stops decode the instruction and name its function", "[kernel][m4]") {
    StopInfo stop;
    SECTION("break 7 after a division") {
        run_site({nop, nop, break7, nop}, false, stop);
        CHECK(stop.reason == StopReason::guest_exception);
        CHECK(stop.message.find("break 7 (the compiler's division-by-zero check) at 0x00300008") !=
              std::string::npos);
        CHECK(stop.message.find("function 0x00300000 of test") != std::string::npos);
        CHECK(stop.message.find("cannot reach") == std::string::npos);
    }
    SECTION("break 6") {
        run_site({break6}, false, stop);
        CHECK(stop.message.find("break 6 (the compiler's overflow check)") != std::string::npos);
    }
    SECTION("a trap") {
        run_site({nop, teq_a0_zero_7}, true, stop);
        CHECK(stop.message.find("teq (code 7 (the compiler's division-by-zero check)) at 0x00300004") !=
              std::string::npos);
    }
    SECTION("a break past a return: data decoded as code") {
        run_site({jr_ra, nop, break7}, false, stop);
        CHECK(stop.message.find("break 7") != std::string::npos);
        CHECK(stop.message.find("cannot reach it: probably data decoded as code") != std::string::npos);
    }
    SECTION("no break at all") {
        run_site({nop, nop, nop}, false, stop);
        CHECK(stop.message.find("no break instruction follows 0x00300000") != std::string::npos);
        CHECK(stop.message.find("probably data decoded as code") != std::string::npos);
    }
}

TEST_CASE("kernel M4: a jump that leaves its function says where it lands", "[kernel][m4]") {
    static std::uint32_t target = 0;
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        rt->dispatchGuestBranch(rdram, ctx, target, k_main + 8, k_main + 16,
                                PS2Runtime::GuestBranchKind::IndirectJump, nullptr);
        guest_return(ctx);
    });
    p.set(k_other, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) { guest_return(ctx); });
    SECTION("into another function") {
        target = k_other + 8;
        TestMachine m(p.table());
        const StopInfo stop = stop_of(m.run_from(k_main));
        CHECK(stop.reason == StopReason::missing_function);
        CHECK(
            stop.message.find("the jump at 0x00300008 in function 0x00300000 lands outside its function, in "
                              "function 0x00300040") != std::string::npos);
    }
    SECTION("inside its own function, at no label") {
        target = k_main + 0x20;
        TestMachine m(p.table());
        const StopInfo stop = stop_of(m.run_from(k_main));
        CHECK(stop.message.find("inside the jumping function 0x00300000") != std::string::npos);
    }
}
