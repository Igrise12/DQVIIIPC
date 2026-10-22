// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xTest/src/ps2_runtime_interrupt_tests.cpp ("negative interrupt-safe EE syscall ids dispatch", "VBlank
// IRQ invocation completes before the resumed base context", "IRQ callbacks use an isolated invocation
// stack", "pending async callbacks execute sequentially on a reusable invocation stack", "iSignalSema defers
// selection until IRQ return", "event-flag completion writes observed bits before strict-priority resume",
// and the Timer2 wake-up). Expected values are upstream's; the harness is ours: interrupts come from the
// InterruptController (a stand-in device or a test event raises them) and run on the handler fiber.
// Upstream's vsync-wait and host-thread stop cases test library fakes and host threading that this port does
// not have; the DI/EI, DMAC masking, chain order and watchdog cases are ours.
#include "runtime/kernel/kernel_abi.h"
#include "runtime/kernel/kernel_layout.h"
#include "runtime/machine/events.h"
#include "support/machine_harness.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace dq8::rt;
using dq8::test::guest_create_sema;
using dq8::test::guest_create_thread;
using dq8::test::guest_request_stop;
using dq8::test::guest_return;
using dq8::test::guest_syscall;
using dq8::test::HostProgram;
using dq8::test::peek;
using dq8::test::poke;
using dq8::test::TestMachine;
using dq8::test::VblankIrqDevice;

constexpr std::uint32_t k_main = 0x00300000u;
constexpr std::uint32_t k_driver = 0x00300100u;
constexpr std::uint32_t k_handler = 0x00300200u;
constexpr std::uint32_t k_handler2 = 0x00300210u;
constexpr std::uint32_t k_param = 0x00001000u;
constexpr std::uint32_t k_result = 0x00001820u;

constexpr std::int32_t sys_add_intc = 0x10;
constexpr std::int32_t sys_remove_intc = 0x11;
constexpr std::int32_t sys_add_dmac = 0x12;
constexpr std::int32_t sys_enable_intc = 0x14;
constexpr std::int32_t sys_enable_dmac = 0x16;
constexpr std::int32_t sys_start_thread = 0x22;
constexpr std::int32_t sys_sleep_thread = 0x32;
constexpr std::int32_t sys_wait_sema = 0x44;
constexpr std::int32_t sys_create_event_flag = 0x50;
constexpr std::int32_t sys_set_event_flag = 0x52;
constexpr std::int32_t sys_wait_event_flag = 0x56;
constexpr std::int32_t sys_disable_dmac_handler = 0x5F;

constexpr std::uint32_t status_eie = 1u << 16;

struct State {
    Machine* machine = nullptr;
    std::vector<int> trace;
    std::vector<std::uint32_t> sps;
    std::uint32_t arg = 0;
    std::int32_t sema = 0;
    std::int32_t flag = 0;
    std::int32_t handler_id = 0;
    std::int32_t results[4]{};
    unsigned handler_runs = 0;
};
State g;

void prepare(TestMachine& m) {
    g = State{};
    g.machine = m.machine.get();
}

StopInfo stop_of(const SliceResult& r) {
    REQUIRE(r.stopped);
    return *r.stop;
}

} // namespace

TEST_CASE("kernel interrupts: the negative interrupt-context numbers dispatch", "[kernel][interrupts]") {
    HostProgram p;
    TestMachine m(p.table());
    m.boot(k_main);
    m.set_word(k_param + 0x00, 0);
    m.set_word(k_param + 0x04, 0);
    m.set_word(k_param + 0x08, 0);
    const std::int32_t flag = m.call(sys_create_event_flag, {k_param});
    REQUIRE(flag > 0);
    InterruptController& irq = m.machine->interrupts();
    CHECK(m.call(-0x1A, {2}) == kabi::ok); // iEnableIntc
    CHECK((irq.intc_mask() & (1u << 2)) != 0);
    CHECK(m.call(-0x1B, {2}) == kabi::ok); // iDisableIntc
    CHECK((irq.intc_mask() & (1u << 2)) == 0);
    CHECK(m.call(-0x1C, {5}) == kabi::ok); // iEnableDmac
    CHECK((irq.dmac_stat() & (1u << (16 + 5))) != 0);
    CHECK(m.call(-0x1D, {5}) == kabi::ok); // iDisableDmac
    CHECK((irq.dmac_stat() & (1u << (16 + 5))) == 0);
    CHECK(m.call(-0x53, {static_cast<std::uint32_t>(flag), 0x6u}) == kabi::ok); // iSetEventFlag
    CHECK(m.call(0x59, {static_cast<std::uint32_t>(flag), k_result}) == kabi::ok);
    CHECK(m.word(k_result + 12) == 0x6u);
    CHECK(m.call(-0x2F) == 0); // GetThreadId from the host: no thread is current
    CHECK(m.call(0x51, {static_cast<std::uint32_t>(flag)}) == kabi::ok);
    // Enabling twice keeps the line enabled (the mask register toggles; the call does not).
    CHECK(m.call(sys_enable_intc, {3}) == kabi::ok);
    CHECK(m.call(sys_enable_intc, {3}) == kabi::ok);
    CHECK((irq.intc_mask() & (1u << 3)) != 0);
}

TEST_CASE("kernel interrupts: a vblank handler completes before the thread it woke resumes",
          "[kernel][interrupts]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(1);
        guest_syscall(rdram, ctx, rt, sys_add_intc, {2, k_handler, 0, 0xCAFEu});
        guest_syscall(rdram, ctx, rt, sys_enable_intc, {2});
        guest_syscall(rdram, ctx, rt, sys_sleep_thread);
        g.trace.push_back(3);
        guest_request_stop(*g.machine);
    });
    p.set(k_handler, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(2);
        g.arg = gpr_u32(*ctx, 5);
        g.results[0] = static_cast<std::int32_t>(gpr_u32(*ctx, 4));
        guest_syscall(rdram, ctx, rt, -0x34,
                      {1}); // iWakeupThread: the waiter becomes ready, nothing switches
        g.trace.push_back(20);
        guest_return(ctx);
    });
    TestMachine m(p.table());
    VblankIrqDevice vblank_irq;
    m.machine->attach(vblank_irq);
    prepare(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.trace == std::vector<int>{1, 2, 20, 3});
    CHECK(g.arg == 0xCAFEu);                                       // the registered argument in $a1
    CHECK(g.results[0] == 2);                                      // the cause in $a0
    CHECK((m.machine->interrupts().intc_stat() & (1u << 2)) == 0); // acknowledged
}

TEST_CASE("kernel interrupts: handlers run on the HLE handler stack, not the registering thread's",
          "[kernel][interrupts]") {
    constexpr std::uint32_t registration_sp = 0x001E0000u;
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        poke(rdram, registration_sp - 16, 0x55667788u);
        set_gpr_lo(*ctx, 29, registration_sp);
        guest_syscall(rdram, ctx, rt, sys_add_intc, {2, k_handler, 0, 0});
        guest_syscall(rdram, ctx, rt, sys_enable_intc, {2});
        guest_syscall(rdram, ctx, rt, sys_sleep_thread);
        g.results[0] = static_cast<std::int32_t>(peek(rdram, registration_sp - 16));
        guest_request_stop(*g.machine);
    });
    p.set(k_handler, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        const std::uint32_t sp = gpr_u32(*ctx, 29);
        g.sps.push_back(sp);
        poke(rdram, sp - 16, 0); // a handler frame
        guest_syscall(rdram, ctx, rt, -0x34, {1});
        guest_return(ctx);
    });
    TestMachine m(p.table());
    VblankIrqDevice vblank_irq;
    m.machine->attach(vblank_irq);
    prepare(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    REQUIRE(g.sps.size() == 1);
    CHECK(g.sps[0] == hle_handler_stack_top);
    CHECK(g.results[0] == 0x55667788); // the registering thread's frame is untouched
}

TEST_CASE("kernel interrupts: a long handler chain runs in order on one reusable stack",
          "[kernel][interrupts]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        for (std::uint32_t i = 0; i < 96; ++i) {
            guest_syscall(rdram, ctx, rt, sys_add_intc, {9, k_handler, 1, i}); // `next` != 0: append
        }
        guest_syscall(rdram, ctx, rt, sys_enable_intc, {9});
        g.machine->interrupts().raise_intc(IntcLine::timer0);
        rt->eeCheckpointDue(); // a safe point: the chain runs here
        g.trace.push_back(-1);
        guest_request_stop(*g.machine);
    });
    p.set(k_handler, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
        g.trace.push_back(static_cast<int>(gpr_u32(*ctx, 5)));
        g.sps.push_back(gpr_u32(*ctx, 29));
        guest_return(ctx);
    });
    TestMachine m(p.table());
    prepare(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    REQUIRE(g.trace.size() == 97);
    for (int i = 0; i < 96; ++i) {
        CHECK(g.trace[static_cast<std::size_t>(i)] == i);
        CHECK(g.sps[static_cast<std::size_t>(i)] == hle_handler_stack_top);
    }
    CHECK(g.trace.back() == -1);
}

TEST_CASE("kernel interrupts: next 0 puts a handler first; removed and disabled handlers do not run",
          "[kernel][interrupts]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        guest_syscall(rdram, ctx, rt, sys_add_intc, {9, k_handler, 1, 10});                     // appended
        const auto first = guest_syscall(rdram, ctx, rt, sys_add_intc, {9, k_handler2, 0, 20}); // first
        const auto third = guest_syscall(rdram, ctx, rt, sys_add_intc, {9, k_handler, 1, 30});  // appended
        guest_syscall(rdram, ctx, rt, sys_enable_intc, {9});
        g.machine->interrupts().raise_intc(IntcLine::timer0);
        rt->eeCheckpointDue();
        guest_syscall(rdram, ctx, rt, sys_remove_intc, {9, static_cast<std::uint32_t>(first)});
        guest_syscall(rdram, ctx, rt, 0x5D, {9, static_cast<std::uint32_t>(third)}); // DisableIntcHandler
        g.machine->interrupts().raise_intc(IntcLine::timer0);
        rt->eeCheckpointDue();
        guest_request_stop(*g.machine);
    });
    const auto record = [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
        g.trace.push_back(static_cast<int>(gpr_u32(*ctx, 5)));
        guest_return(ctx);
    };
    p.set(k_handler, record);
    p.set(k_handler2, record);
    TestMachine m(p.table());
    prepare(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.trace == std::vector<int>{20, 10, 30, 10});
}

TEST_CASE("kernel interrupts: iSignalSema defers selection until the handler returns",
          "[kernel][interrupts]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(1);
        g.sema = guest_create_sema(rdram, ctx, rt, k_param + 0x100, 0, 1);
        guest_syscall(rdram, ctx, rt, sys_add_dmac, {5, k_handler, 0, 0});
        guest_syscall(rdram, ctx, rt, sys_enable_dmac, {5});
        const auto driver = guest_create_thread(rdram, ctx, rt, k_param, k_driver, 0x1C000u, 0x1000u, 10);
        guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(driver), 0});
        g.results[0] = guest_syscall(rdram, ctx, rt, sys_wait_sema, {static_cast<std::uint32_t>(g.sema)});
        g.trace.push_back(5);
        guest_request_stop(*g.machine);
    });
    p.set(k_driver, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
        g.trace.push_back(2);
        g.machine->interrupts().raise_dmac(DmaChannel::sif0); // taken at the run loop's safe point
        guest_return(ctx);
    });
    p.set(k_handler, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(3);
        guest_syscall(rdram, ctx, rt, -0x43, {static_cast<std::uint32_t>(g.sema)}); // iSignalSema
        g.trace.push_back(4);
        guest_return(ctx);
    });
    TestMachine m(p.table());
    prepare(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.trace == std::vector<int>{1, 2, 3, 4, 5});
    CHECK(g.results[0] == g.sema);
    REQUIRE(m.kernel().semaphore(g.sema) != nullptr);
    CHECK(m.kernel().semaphore(g.sema)->count == 0);
    CHECK(m.kernel().semaphore(g.sema)->waiters.empty());
    CHECK((m.machine->interrupts().dmac_stat() & (1u << 5)) == 0); // CIS acknowledged
}

TEST_CASE("kernel interrupts: an event-flag completion writes the observed bits before the waiter resumes",
          "[kernel][interrupts]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(1);
        poke(rdram, k_param + 0x100, 0);
        poke(rdram, k_param + 0x104, 0);
        poke(rdram, k_param + 0x108, 0);
        g.flag = guest_syscall(rdram, ctx, rt, sys_create_event_flag, {k_param + 0x100});
        const auto producer = guest_create_thread(rdram, ctx, rt, k_param, k_driver, 0x1D000u, 0x1000u, 10);
        guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(producer), 0});
        g.results[0] = guest_syscall(rdram, ctx, rt, sys_wait_event_flag,
                                     {static_cast<std::uint32_t>(g.flag), 0x2u,
                                      kabi::event_mode_or | kabi::event_mode_clear, k_result});
        g.trace.push_back(3);
        guest_request_stop(*g.machine);
    });
    p.set(k_driver, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(2);
        guest_syscall(rdram, ctx, rt, sys_set_event_flag, {static_cast<std::uint32_t>(g.flag), 0x6u});
        g.trace.push_back(-2); // never reached: the waiter outranks us and ends the run
        guest_return(ctx);
    });
    TestMachine m(p.table());
    prepare(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.trace == std::vector<int>{1, 2, 3});
    CHECK(g.results[0] == kabi::ok);
    CHECK(m.word(k_result) == 0x6u);
    CHECK(m.kernel().event_flag(g.flag)->bits == 0x4u); // WEF_CLEAR removed only the requested bit
}

TEST_CASE("kernel interrupts: a timer interrupt wakes a DelayThread-style semaphore wait",
          "[kernel][interrupts]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(1);
        g.sema = guest_create_sema(rdram, ctx, rt, k_param + 0x100, 0, 1);
        guest_syscall(rdram, ctx, rt, sys_add_intc, {11, k_handler, 0, 0});
        guest_syscall(rdram, ctx, rt, sys_enable_intc, {11});
        g.results[0] = guest_syscall(rdram, ctx, rt, sys_wait_sema, {static_cast<std::uint32_t>(g.sema)});
        g.trace.push_back(3);
        guest_request_stop(*g.machine);
    });
    p.set(k_handler, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(2);
        guest_syscall(rdram, ctx, rt, -0x43, {static_cast<std::uint32_t>(g.sema)});
        guest_return(ctx);
    });
    TestMachine m(p.table());
    prepare(m);
    m.boot(k_main);
    // A timer-2 compare in 2000 cycles, as session B's timer device would raise it.
    m.machine->events().schedule(2000, "test-timer2",
                                 [&m](Cycles) { m.machine->interrupts().raise_intc(IntcLine::timer2); });
    CHECK(stop_of(m.run()).reason == StopReason::requested);
    CHECK(g.trace == std::vector<int>{1, 2, 3});
    CHECK(g.results[0] == g.sema);
}

TEST_CASE("kernel interrupts: DI holds an interrupt until EI, and the handler sees the interrupted thread",
          "[kernel][interrupts]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        guest_syscall(rdram, ctx, rt, sys_add_intc, {9, k_handler, 0, 0});
        guest_syscall(rdram, ctx, rt, sys_enable_intc, {9});
        ctx->cop0_status &= ~status_eie; // DI, inline as generated code does it
        for (int i = 0; i < 1000; ++i) {
            rt->eeCheckpointDue(); // 32000 cycles of back-edges; the interrupt arrives at cycle 5000
        }
        g.trace.push_back(static_cast<int>(g.handler_runs)); // still 0
        g.results[1] = (g.machine->interrupts().intc_stat() & (1u << 9)) != 0 ? 1 : 0;
        ctx->cop0_status |= status_eie; // EI
        rt->eeCheckpointDue();          // taken here
        g.trace.push_back(static_cast<int>(g.handler_runs));
        guest_request_stop(*g.machine);
    });
    p.set(k_handler, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        ++g.handler_runs;
        g.results[0] = guest_syscall(rdram, ctx, rt, -0x2F); // GetThreadId in interrupt context
        guest_return(ctx);
    });
    TestMachine m(p.table());
    prepare(m);
    m.boot(k_main);
    m.machine->events().schedule(5000, "test-timer0",
                                 [&m](Cycles) { m.machine->interrupts().raise_intc(IntcLine::timer0); });
    CHECK(stop_of(m.run()).reason == StopReason::requested);
    CHECK(g.trace == std::vector<int>{0, 1});
    CHECK(g.results[1] == 1); // pending the whole time
    CHECK(g.results[0] == 1); // the interrupted thread
}

TEST_CASE("kernel interrupts: DMAC causes need their mask bit, and a disabled handler is skipped",
          "[kernel][interrupts]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.handler_id = guest_syscall(rdram, ctx, rt, sys_add_dmac, {5, k_handler, 0, 0});
        InterruptController& irq = g.machine->interrupts();
        irq.raise_dmac(DmaChannel::sif0); // masked: status only
        rt->eeCheckpointDue();
        g.trace.push_back(static_cast<int>(g.handler_runs)); // 0
        guest_syscall(rdram, ctx, rt, sys_enable_dmac, {5}); // now pending; taken after this call
        g.trace.push_back(static_cast<int>(g.handler_runs)); // 1
        guest_syscall(rdram, ctx, rt, sys_disable_dmac_handler,
                      {5, static_cast<std::uint32_t>(g.handler_id)});
        irq.raise_dmac(DmaChannel::sif0);
        rt->eeCheckpointDue();
        g.trace.push_back(static_cast<int>(g.handler_runs)); // still 1, and the cause was acknowledged
        g.results[0] = (irq.dmac_stat() & (1u << 5)) != 0 ? 1 : 0;
        guest_request_stop(*g.machine);
    });
    p.set(k_handler, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
        ++g.handler_runs;
        guest_return(ctx);
    });
    TestMachine m(p.table());
    prepare(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.trace == std::vector<int>{0, 1, 1});
    CHECK(g.results[0] == 0);
}

TEST_CASE("kernel interrupts: the watchdog stops a machine where nothing makes progress",
          "[kernel][interrupts]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        guest_syscall(rdram, ctx, rt, sys_add_intc, {2, k_handler, 0, 0}); // can fire: not a deadlock
        guest_syscall(rdram, ctx, rt, sys_enable_intc, {2});
        guest_syscall(rdram, ctx, rt, sys_sleep_thread); // nobody wakes it
        guest_return(ctx);
    });
    p.set(k_handler, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
        ++g.handler_runs;
        guest_return(ctx);
    });
    MachineConfig config;
    config.no_progress_fields = 5;
    TestMachine m(p.table(), config);
    VblankIrqDevice vblank_irq;
    m.machine->attach(vblank_irq);
    prepare(m);
    m.boot(k_main);
    RunLimits limits;
    limits.until_vblank = 3;
    const SliceResult first = m.run(limits); // a slice ends on its limit; the run goes on
    CHECK_FALSE(first.stopped);
    CHECK(first.vblank == 3);
    const StopInfo stop = stop_of(m.run({}));
    CHECK(stop.reason == StopReason::no_progress);
    CHECK(stop.thread_id == 0); // raised on the scheduler's stack, at a vblank
    CHECK(stop.message.find("thread 1: waiting") != std::string::npos);
    CHECK(stop.message.find("INTC handler") != std::string::npos);
    CHECK(g.handler_runs >= 4); // the handler kept running; it is not progress
}

TEST_CASE("kernel interrupts: a stop inside a handler reports the handler fiber and its context",
          "[kernel][interrupts]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        guest_syscall(rdram, ctx, rt, sys_add_intc, {9, k_handler, 0, 0x77u});
        guest_syscall(rdram, ctx, rt, sys_enable_intc, {9});
        g.machine->interrupts().raise_intc(IntcLine::timer0);
        rt->eeCheckpointDue();
        guest_return(ctx);
    });
    p.set(k_handler, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        ctx->pc = k_handler + 0x10;
        guest_syscall(rdram, ctx, rt, 0x7E); // a number no kernel call has
        guest_return(ctx);
    });
    TestMachine m(p.table());
    prepare(m);
    const StopInfo stop = stop_of(m.run_from(k_main));
    CHECK(stop.reason == StopReason::syscall);
    CHECK(stop.thread_id == handler_fiber_id);
    CHECK(m.ctx().pc == k_handler + 0x10); // EeRuntime::context(): the handler's registers
    CHECK(gpr_u32(m.ctx(), 5) == 0x77u);
    CHECK(gpr_u32(m.ctx(), 29) == hle_handler_stack_top);
}
