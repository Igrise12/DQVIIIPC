// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors: the semaphore, event-flag and
// alarm cases of ps2xTest/src/ps2_runtime_kernel_tests.cpp ("semaphore signal, delete, and release complete
// blocked contexts with exact results", the alarm and cache part of "ReleaseAlarm aliases CancelAlarm and
// cache syscalls succeed") and ps2xTest/src/ps2_runtime_interrupt_tests.cpp ("PollEventFlag WEF_CLEAR clears
// only matched bits"). Expected values are upstream's; the harness is ours. The argument checks, the status
// blocks and the alarm timing on hblanks are ours.
#include "runtime/kernel/kernel_abi.h"
#include "runtime/machine/clock.h"
#include "support/machine_harness.h"

#include <catch2/catch_test_macros.hpp>
#include <vector>

namespace {

using namespace dq8::rt;
using dq8::test::guest_create_sema;
using dq8::test::guest_create_thread;
using dq8::test::guest_request_stop;
using dq8::test::guest_return;
using dq8::test::guest_syscall;
using dq8::test::HostProgram;
using dq8::test::TestMachine;

constexpr std::uint32_t k_main = 0x00300000u;
constexpr std::uint32_t k_helper = 0x00300100u;
constexpr std::uint32_t k_alarm = 0x00300200u;
constexpr std::uint32_t k_param = 0x00001000u;
constexpr std::uint32_t k_status = 0x00001400u;

constexpr std::int32_t sys_start_thread = 0x22;
constexpr std::int32_t sys_sleep_thread = 0x32;
constexpr std::int32_t sys_release_wait = 0x2D;
constexpr std::int32_t sys_create_sema = 0x40;
constexpr std::int32_t sys_delete_sema = 0x41;
constexpr std::int32_t sys_signal_sema = 0x42;
constexpr std::int32_t sys_wait_sema = 0x44;
constexpr std::int32_t sys_poll_sema = 0x45;
constexpr std::int32_t sys_refer_sema = 0x47;
constexpr std::int32_t sys_create_event_flag = 0x50;
constexpr std::int32_t sys_delete_event_flag = 0x51;
constexpr std::int32_t sys_set_event_flag = 0x52;
constexpr std::int32_t sys_wait_event_flag = 0x56;
constexpr std::int32_t sys_poll_event_flag = 0x57;
constexpr std::int32_t sys_refer_event_flag = 0x59;
constexpr std::int32_t sys_set_alarm = 0x18;
constexpr std::int32_t sys_cancel_alarm = 0x19;

enum class Op { signal, remove, release };

struct State {
    Machine* machine = nullptr;
    Op op = Op::signal;
    std::int32_t sema = 0;
    std::int32_t flag = 0;
    std::int32_t results[4]{};
    std::uint32_t alarm_args[4]{};
    Cycles set_at = 0;
    Cycles fired_at = 0;
    unsigned alarm_runs = 0;
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

std::int32_t create_flag(TestMachine& m, std::uint32_t attr, std::uint32_t bits) {
    m.set_word(k_param + 0x00, attr);
    m.set_word(k_param + 0x04, 0);
    m.set_word(k_param + 0x08, bits);
    return m.call(sys_create_event_flag, {k_param});
}

// The number of hblank boundaries (scan lines) started by cycle t.
std::uint64_t lines_started(const VideoTiming& v, Cycles t) {
    std::uint64_t n = 0;
    while (v.line_start(n) <= t) {
        ++n;
    }
    return n;
}

} // namespace

TEST_CASE("kernel sync: SignalSema, DeleteSema and ReleaseWaitThread complete a waiter with exact results",
          "[kernel][sync]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.sema = guest_create_sema(rdram, ctx, rt, k_param + 0x100, 0, 1);
        const auto h = guest_create_thread(rdram, ctx, rt, k_param, k_helper, 0x20000u, 0x800u, 10);
        guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(h), 0});
        g.results[0] = guest_syscall(rdram, ctx, rt, sys_wait_sema, {static_cast<std::uint32_t>(g.sema)});
        guest_request_stop(*g.machine);
    });
    p.set(k_helper, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        const auto id = static_cast<std::uint32_t>(g.sema);
        switch (g.op) {
        case Op::signal:
            g.results[1] = guest_syscall(rdram, ctx, rt, sys_signal_sema, {id});
            break;
        case Op::remove:
            g.results[1] = guest_syscall(rdram, ctx, rt, sys_delete_sema, {id});
            break;
        case Op::release:
            g.results[1] = guest_syscall(rdram, ctx, rt, sys_release_wait, {1});
            break;
        }
        guest_return(ctx);
    });
    for (const Op op : {Op::signal, Op::remove, Op::release}) {
        TestMachine m(p.table());
        prepare(m);
        g.op = op;
        CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
        // The waiter (priority 0) preempts the helper inside its call, before the helper sees its own result.
        CHECK(g.results[1] == 0);
        switch (op) {
        case Op::signal:
            CHECK(g.results[0] == g.sema); // the waiter receives the semaphore id
            CHECK(m.kernel().semaphore(g.sema)->count == 0);
            break;
        case Op::remove:
            CHECK(g.results[0] == kabi::wait_deleted);
            CHECK(m.kernel().semaphore(g.sema) == nullptr);
            break;
        case Op::release:
            CHECK(g.results[0] == kabi::release_wait);
            CHECK(m.kernel().semaphore(g.sema)->waiters.empty());
            break;
        }
    }
}

TEST_CASE("kernel sync: semaphore counts, limits and the status block", "[kernel][sync]") {
    HostProgram p;
    TestMachine m(p.table());
    m.boot(k_main);
    m.set_word(k_param + 0x04, 2);    // max
    m.set_word(k_param + 0x08, 1);    // initial
    m.set_word(k_param + 0x10, 0x5u); // attr
    m.set_word(k_param + 0x14, 0x6u); // option
    const std::int32_t sema = m.call(sys_create_sema, {k_param});
    const auto u = static_cast<std::uint32_t>(sema);
    CHECK(m.call(sys_signal_sema, {u}) == sema);
    CHECK(m.call(sys_signal_sema, {u}) == kabi::semaphore_overflow);
    CHECK(m.call(sys_poll_sema, {u}) == sema);
    CHECK(m.call(-0x46, {u}) == sema); // iPollSema
    CHECK(m.call(sys_poll_sema, {u}) == kabi::semaphore_zero);
    CHECK(m.call(-0x43, {u}) == sema); // iSignalSema
    CHECK(m.call(sys_refer_sema, {u, k_status}) == kabi::ok);
    CHECK(m.word(k_status + 0x00) == 1u);
    CHECK(m.word(k_status + 0x04) == 2u);
    CHECK(m.word(k_status + 0x08) == 1u);
    CHECK(m.word(k_status + 0x0C) == 0u);
    CHECK(m.word(k_status + 0x10) == 0x5u);
    CHECK(m.word(k_status + 0x14) == 0x6u);
    CHECK(m.call(sys_refer_sema, {u, 0}) == kabi::error);
    m.set_word(k_param + 0x04, 0); // a maximum of 0 is refused
    CHECK(m.call(sys_create_sema, {k_param}) == kabi::error);
    CHECK(m.call(-0x49, {u}) == sema); // iDeleteSema
    CHECK(m.call(sys_signal_sema, {u}) == kabi::unknown_semaphore_id);
}

TEST_CASE("kernel sync: PollEventFlag with WEF_CLEAR clears only the matched bits", "[kernel][sync]") {
    HostProgram p;
    TestMachine m(p.table());
    m.boot(k_main);
    const std::int32_t flag = create_flag(m, 0, 0x7u);
    REQUIRE(flag > 0);
    const auto u = static_cast<std::uint32_t>(flag);
    CHECK(m.call(sys_poll_event_flag, {u, 0x1u, kabi::event_mode_or | kabi::event_mode_clear, k_status}) ==
          kabi::ok);
    CHECK(m.word(k_status) == 0x7u); // the bits before the clear
    CHECK(m.call(sys_refer_event_flag, {u, k_status + 0x10}) == kabi::ok);
    CHECK(m.word(k_status + 0x10 + kabi::event_flag_param::current_bits) == 0x6u);
    CHECK(m.word(k_status + 0x10 + kabi::event_flag_param::init_bits) == 0x7u);
    CHECK(m.call(sys_poll_event_flag, {u, 0x1u, kabi::event_mode_or, 0}) == kabi::event_condition);
    CHECK(m.call(sys_delete_event_flag, {u}) == kabi::ok);
    CHECK(m.call(sys_set_event_flag, {u, 1}) == kabi::unknown_event_flag_id);
}

TEST_CASE("kernel sync: event-flag argument checks", "[kernel][sync]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        const auto h = guest_create_thread(rdram, ctx, rt, k_param + 0x40, k_helper, 0x20000u, 0x800u, 10);
        guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(h), 0});
        // A single-waiter flag: the helper's poll sees us waiting.
        g.results[0] = guest_syscall(rdram, ctx, rt, sys_wait_event_flag,
                                     {static_cast<std::uint32_t>(g.flag), 0x1u, 0u, k_status});
        guest_request_stop(*g.machine);
    });
    p.set(k_helper, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        const auto u = static_cast<std::uint32_t>(g.flag);
        g.results[1] = guest_syscall(rdram, ctx, rt, sys_poll_event_flag, {u, 0x1u, 0u, 0u});
        g.results[2] = guest_syscall(rdram, ctx, rt, sys_set_event_flag, {u, 0x3u});
        guest_return(ctx);
    });
    TestMachine m(p.table());
    prepare(m);
    m.boot(k_main);
    g.flag = create_flag(m, 0, 0);
    const auto u = static_cast<std::uint32_t>(g.flag);
    CHECK(m.call(sys_poll_event_flag, {u, 0x1u, 0x40u, 0}) == kabi::illegal_mode);
    CHECK(m.call(sys_poll_event_flag, {u, 0u, 0u, 0}) == kabi::event_illegal_pattern);
    CHECK(m.call(sys_poll_event_flag, {0x7FFF, 0x1u, 0u, 0}) == kabi::unknown_event_flag_id);
    CHECK(m.call(sys_poll_event_flag, {u, 0x1u, 0u, 0x03000000u}) == kabi::error);
    CHECK(stop_of(m.run()).reason == StopReason::requested);
    CHECK(g.results[1] == kabi::event_multi);
    CHECK(g.results[2] == kabi::ok);
    CHECK(g.results[0] == kabi::ok);
    CHECK(m.word(k_status) == 0x3u);
}

TEST_CASE("kernel sync: ReleaseAlarm cancels like CancelAlarm, and the cache calls succeed",
          "[kernel][sync]") {
    HostProgram p;
    p.set(k_alarm, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) { guest_return(ctx); });
    TestMachine m(p.table());
    m.boot(k_main);
    const std::int32_t alarm = m.call(sys_set_alarm, {0xFFFFu, k_alarm, 0});
    CHECK(alarm > 0);
    CHECK(m.named("ReleaseAlarm", {static_cast<std::uint32_t>(alarm)}) == kabi::ok);
    CHECK(m.call(sys_cancel_alarm, {static_cast<std::uint32_t>(alarm)}) == kabi::error);
    CHECK(m.kernel().alarm_count() == 0);
    CHECK(m.call(sys_set_alarm, {10u, 0, 0}) == kabi::error); // no handler
    CHECK(m.call(0x61) == kabi::ok);                          // EnableCache
    CHECK(m.call(0x62) == kabi::ok);                          // DisableCache
    CHECK(m.call(-0x68, {0}) == kabi::ok);                    // iFlushCache(WRITEBACK_DCACHE)
}

TEST_CASE("kernel sync: an alarm fires on its hblank with its arguments, on the handler fiber",
          "[kernel][sync]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        set_gpr_lo(*ctx, 28, 0x00123450u); // the handler runs with the caller's $gp
        g.set_at = g.machine->clock().now();
        g.results[0] = guest_syscall(rdram, ctx, rt, sys_set_alarm, {10u, k_alarm, 0xABCDu});
        g.results[1] = guest_syscall(rdram, ctx, rt, sys_sleep_thread);
        guest_request_stop(*g.machine);
    });
    p.set(k_alarm, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        ++g.alarm_runs;
        g.fired_at = g.machine->clock().now();
        g.alarm_args[0] = gpr_u32(*ctx, 4);
        g.alarm_args[1] = gpr_u32(*ctx, 5);
        g.alarm_args[2] = gpr_u32(*ctx, 6);
        g.alarm_args[3] = gpr_u32(*ctx, 28);
        g.results[2] = g.machine->scheduler()->in_handler() ? 1 : 0;
        guest_syscall(rdram, ctx, rt, -0x34, {1}); // iWakeupThread
        guest_return(ctx);
    });
    TestMachine m(p.table());
    prepare(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.results[0] > 0);
    CHECK(g.results[1] == kabi::ok);
    CHECK(g.alarm_runs == 1);
    CHECK(g.results[2] == 1);
    CHECK(g.alarm_args[0] == static_cast<std::uint32_t>(g.results[0])); // alarm id
    CHECK(g.alarm_args[1] == 10u);                                      // ticks
    CHECK(g.alarm_args[2] == 0xABCDu);                                  // argument
    CHECK(g.alarm_args[3] == 0x00123450u);                              // the caller's $gp
    // The service ran after the syscall's charge; the alarm is due on the 10th hblank boundary after it.
    const VideoTiming& v = m.machine->video();
    const Cycles service_at = g.set_at + m.machine->cost().kernel_call;
    const Cycles due = v.line_start(lines_started(v, service_at) - 1 + 10);
    CHECK(g.fired_at >= due);
    CHECK(g.fired_at < due + 1000); // taken at the first safe point after it (the idle jump lands on it)
}

TEST_CASE("kernel sync: a cancelled alarm never runs", "[kernel][sync]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        const auto id = guest_syscall(rdram, ctx, rt, sys_set_alarm, {5u, k_alarm, 0});
        g.results[0] = guest_syscall(rdram, ctx, rt, sys_cancel_alarm, {static_cast<std::uint32_t>(id)});
        guest_syscall(rdram, ctx, rt, sys_sleep_thread);
        guest_return(ctx);
    });
    p.set(k_alarm, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
        ++g.alarm_runs;
        guest_return(ctx);
    });
    TestMachine m(p.table());
    prepare(m);
    m.boot(k_main);
    RunLimits limits;
    limits.until_vblank = 3;
    const SliceResult r = m.run(limits);
    // Sleeping with no handler, alarm or ready thread left: nothing can wake it.
    CHECK(stop_of(r).reason == StopReason::deadlock);
    CHECK(r.stop->message.find("thread 1: waiting") != std::string::npos);
    CHECK(g.results[0] == kabi::ok);
    CHECK(g.alarm_runs == 0);
}
