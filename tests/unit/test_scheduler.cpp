// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors: the scheduling cases of
// ps2xTest/src/ps2_runtime_kernel_tests.cpp ("EE scheduler selects absolute priority then FIFO",
// "RotateThreadReadyQueue is the only same-priority rotation", "starting a strictly higher-priority thread
// preempts immediately", "semaphore waiters are FIFO and signal transfers one token directly", "event waiters
// are FIFO and clear modes apply before testing the next waiter"). The expected traces and results are
// upstream's; the harness is ours: the guest code is host functions on a real Machine, the boot thread
// creates the other threads with kernel calls, and a blocking call returns inline instead of resuming at a
// second entry.
#include "runtime/kernel/kernel_abi.h"
#include "support/machine_harness.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using namespace dq8::rt;
using dq8::test::guest_create_sema;
using dq8::test::guest_create_thread;
using dq8::test::guest_request_stop;
using dq8::test::guest_return;
using dq8::test::guest_syscall;
using dq8::test::HostProgram;
using dq8::test::poke;
using dq8::test::TestMachine;

constexpr std::uint32_t k_main = 0x00300000u;
constexpr std::uint32_t k_a = 0x00300100u;
constexpr std::uint32_t k_b = 0x00300200u;
constexpr std::uint32_t k_high = 0x00300300u;
constexpr std::uint32_t k_signal = 0x00300400u;
constexpr std::uint32_t k_param = 0x00001000u;
constexpr std::uint32_t k_flag_a = 0x00001A00u;
constexpr std::uint32_t k_flag_b = 0x00001A04u;

constexpr std::int32_t sys_start_thread = 0x22;
constexpr std::int32_t sys_exit_thread = 0x23;
constexpr std::int32_t sys_rotate = 0x2B;
constexpr std::int32_t sys_signal_sema = 0x42;
constexpr std::int32_t sys_wait_sema = 0x44;
constexpr std::int32_t sys_create_event_flag = 0x50;
constexpr std::int32_t sys_set_event_flag = 0x52;
constexpr std::int32_t sys_wait_event_flag = 0x56;
constexpr std::int32_t sys_poll_event_flag = 0x57;

// One test's state (the guest functions are captureless).
struct State {
    Machine* machine = nullptr;
    std::vector<int> trace;
    std::vector<std::thread::id> hosts;
    std::int32_t ids[4]{};
    std::int32_t sema = 0;
    std::int32_t result_a = 0;
    std::int32_t result_b = 0;
};
State g;

void note(int value) {
    g.trace.push_back(value);
    g.hosts.push_back(std::this_thread::get_id());
}

void stop_when(std::size_t size) {
    if (g.trace.size() >= size) {
        guest_request_stop(*g.machine);
    }
}

TestMachine& start(TestMachine& m) {
    g = State{};
    g.machine = m.machine.get();
    return m;
}

// WaitEventFlag(flag, bit 0, OR | CLEAR), noting `before` and `after` around it.
void event_waiter(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt, int before, int after,
                  std::uint32_t result) {
    note(before);
    guest_syscall(
        rdram, ctx, rt, sys_wait_event_flag,
        {static_cast<std::uint32_t>(g.ids[0]), 1u, kabi::event_mode_or | kabi::event_mode_clear, result});
    note(after);
    guest_return(ctx);
}

StopInfo stop_of(const SliceResult& r) {
    REQUIRE(r.stopped);
    return *r.stop;
}

} // namespace

TEST_CASE("scheduler: absolute priority, then FIFO within a priority", "[scheduler]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        const auto low = guest_create_thread(rdram, ctx, rt, k_param, k_a, 0x20000u, 0x800u, 20);
        const auto high_a = guest_create_thread(rdram, ctx, rt, k_param, k_a, 0x21000u, 0x800u, 5);
        const auto high_b = guest_create_thread(rdram, ctx, rt, k_param, k_b, 0x22000u, 0x800u, 5);
        for (const auto id : {low, high_a, high_b}) {
            guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(id), 0});
        }
        note(1); // the boot thread runs at priority 0: nothing preempted it
        guest_syscall(rdram, ctx, rt, sys_exit_thread);
    });
    p.set(k_a, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
        note(10);
        stop_when(4);
        guest_return(ctx); // $ra 0: the thread exits
    });
    p.set(k_b, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
        note(20);
        stop_when(4);
        guest_return(ctx);
    });
    TestMachine m(p.table());
    start(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.trace == std::vector<int>{1, 10, 20, 10});
    for (const auto& id : g.hosts) {
        CHECK(id == std::this_thread::get_id()); // one machine thread runs every guest thread
    }
}

TEST_CASE("scheduler: RotateThreadReadyQueue is the only same-priority rotation", "[scheduler]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        const auto first = guest_create_thread(rdram, ctx, rt, k_param, k_a, 0x20000u, 0x800u, 5);
        const auto second = guest_create_thread(rdram, ctx, rt, k_param, k_b, 0x21000u, 0x800u, 5);
        guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(first), 0});
        guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(second), 0});
        note(1);
        guest_syscall(rdram, ctx, rt, sys_exit_thread);
    });
    p.set(k_a, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        note(10);
        guest_syscall(rdram, ctx, rt, sys_rotate, {5}); // moves itself behind its FIFO peer
        note(11);
        stop_when(4);
        guest_return(ctx);
    });
    p.set(k_b, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
        note(20);
        stop_when(4);
        guest_return(ctx);
    });
    TestMachine m(p.table());
    start(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.trace == std::vector<int>{1, 10, 20, 11});
}

TEST_CASE("scheduler: starting a strictly higher-priority thread preempts immediately", "[scheduler]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        const auto low = guest_create_thread(rdram, ctx, rt, k_param, k_a, 0x20000u, 0x800u, 20);
        g.ids[0] = guest_create_thread(rdram, ctx, rt, k_param, k_high, 0x21000u, 0x800u, 5);
        guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(low), 0});
        note(1);
        guest_syscall(rdram, ctx, rt, sys_exit_thread);
    });
    p.set(k_a, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        note(30);
        guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(g.ids[0]), 0});
        note(31); // only after the started thread ran
        stop_when(4);
        guest_return(ctx);
    });
    p.set(k_high, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
        note(5);
        guest_return(ctx);
    });
    TestMachine m(p.table());
    start(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.trace == std::vector<int>{1, 30, 5, 31});
}

TEST_CASE("scheduler: semaphore waiters are FIFO and a signal hands one token directly", "[scheduler]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.sema = guest_create_sema(rdram, ctx, rt, k_param + 0x100, 0, 1);
        const auto a = guest_create_thread(rdram, ctx, rt, k_param, k_a, 0x20000u, 0x800u, 5);
        const auto b = guest_create_thread(rdram, ctx, rt, k_param, k_b, 0x21000u, 0x800u, 5);
        const auto s = guest_create_thread(rdram, ctx, rt, k_param, k_signal, 0x22000u, 0x800u, 20);
        for (const auto id : {a, b, s}) {
            guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(id), 0});
        }
        note(1);
        guest_syscall(rdram, ctx, rt, sys_exit_thread);
    });
    p.set(k_a, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        note(10);
        g.result_a = guest_syscall(rdram, ctx, rt, sys_wait_sema, {static_cast<std::uint32_t>(g.sema)});
        note(11);
        guest_return(ctx);
    });
    p.set(k_b, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        note(20);
        g.result_b = guest_syscall(rdram, ctx, rt, sys_wait_sema, {static_cast<std::uint32_t>(g.sema)});
        note(21);
        guest_request_stop(*g.machine);
    });
    p.set(k_signal, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        note(30);
        guest_syscall(rdram, ctx, rt, sys_signal_sema, {static_cast<std::uint32_t>(g.sema)});
        note(31);
        guest_syscall(rdram, ctx, rt, sys_signal_sema, {static_cast<std::uint32_t>(g.sema)});
        guest_return(ctx);
    });
    TestMachine m(p.table());
    start(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.trace == std::vector<int>{1, 10, 20, 30, 11, 31, 21});
    CHECK(g.result_a == g.sema); // each waiter receives the semaphore id
    CHECK(g.result_b == g.sema);
    REQUIRE(m.kernel().semaphore(g.sema) != nullptr);
    CHECK(m.kernel().semaphore(g.sema)->count == 0); // a direct hand-off never increments the count
}

TEST_CASE("scheduler: event waiters are FIFO and a clear mode applies before the next waiter is tested",
          "[scheduler]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        poke(rdram, k_param + 0x100, kabi::event_attr_multi); // attr
        poke(rdram, k_param + 0x104, 0);                      // option
        poke(rdram, k_param + 0x108, 0);                      // initial bits
        g.ids[0] = guest_syscall(rdram, ctx, rt, sys_create_event_flag, {k_param + 0x100});
        const auto a = guest_create_thread(rdram, ctx, rt, k_param, k_a, 0x21000u, 0x800u, 5);
        const auto b = guest_create_thread(rdram, ctx, rt, k_param, k_b, 0x22000u, 0x800u, 5);
        const auto s = guest_create_thread(rdram, ctx, rt, k_param, k_signal, 0x23000u, 0x800u, 10);
        for (const auto id : {a, b, s}) {
            guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(id), 0});
        }
        guest_syscall(rdram, ctx, rt, sys_exit_thread);
    });
    p.set(k_a, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        event_waiter(rdram, ctx, rt, 1, 4, k_flag_a);
    });
    p.set(k_b, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        event_waiter(rdram, ctx, rt, 2, 6, k_flag_b);
    });
    p.set(k_signal, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        note(3);
        guest_syscall(rdram, ctx, rt, sys_set_event_flag, {static_cast<std::uint32_t>(g.ids[0]), 1u});
        note(5);
        guest_syscall(rdram, ctx, rt, sys_set_event_flag, {static_cast<std::uint32_t>(g.ids[0]), 1u});
        note(7);
        guest_request_stop(*g.machine);
    });
    TestMachine m(p.table());
    start(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.trace == std::vector<int>{1, 2, 3, 4, 5, 6, 7});
    CHECK(m.word(k_flag_a) == 1u); // the bits each waiter saw before its clear
    CHECK(m.word(k_flag_b) == 1u);
    REQUIRE(m.kernel().event_flag(g.ids[0]) != nullptr);
    CHECK(m.kernel().event_flag(g.ids[0])->bits == 0u);

    // WEF_CLEAR_ALL: completes on any requested bit, reports the bits before clearing all of them.
    m.set_word(k_param + 0x200, kabi::event_attr_multi);
    m.set_word(k_param + 0x204, 0);
    m.set_word(k_param + 0x208, 0x7u);
    const std::int32_t clear_all = m.call(sys_create_event_flag, {k_param + 0x200});
    CHECK(m.call(sys_poll_event_flag, {static_cast<std::uint32_t>(clear_all), 0x2u,
                                       kabi::event_mode_or | kabi::event_mode_clear_all, k_param + 0x300}) ==
          kabi::ok);
    CHECK(m.word(k_param + 0x300) == 0x7u);
    CHECK(m.kernel().event_flag(clear_all)->bits == 0u);
}
