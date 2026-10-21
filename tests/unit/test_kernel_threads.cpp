// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors: the thread cases of
// ps2xTest/src/ps2_runtime_kernel_tests.cpp ("CreateThread and CreateSema decode the exact PS2SDK EE
// layouts", "thread lifecycle, nested suspend, WAIT-SUSPEND, and wakeup count are centralized", the thread
// part of "setup heap and thread invalid ids use documented kernel errors"). Expected values are upstream's;
// the harness is ours (kernel calls on a real Machine; the blocking part runs on guest threads). The cases
// after those are ours.
#include "runtime/kernel/kernel_abi.h"
#include "support/machine_harness.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
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
using dq8::test::TestMachine;

constexpr std::uint32_t k_main = 0x00300000u;
constexpr std::uint32_t k_helper = 0x00300100u;
constexpr std::uint32_t k_high = 0x00300300u;
constexpr std::uint32_t k_param = 0x00001000u;
constexpr std::uint32_t k_status = 0x00001400u;

constexpr std::int32_t sys_create_thread = 0x20;
constexpr std::int32_t sys_delete_thread = 0x21;
constexpr std::int32_t sys_start_thread = 0x22;
constexpr std::int32_t sys_exit_delete_thread = 0x24;
constexpr std::int32_t sys_terminate_thread = 0x25;
constexpr std::int32_t sys_change_priority = 0x29;
constexpr std::int32_t sys_get_thread_id = 0x2F;
constexpr std::int32_t sys_refer_thread_status = 0x30;
constexpr std::int32_t sys_sleep_thread = 0x32;
constexpr std::int32_t sys_wakeup_thread = 0x33;
constexpr std::int32_t sys_cancel_wakeup = 0x35;
constexpr std::int32_t sys_suspend_thread = 0x37;
constexpr std::int32_t sys_resume_thread = 0x39;
constexpr std::int32_t sys_create_sema = 0x40;
constexpr std::int32_t sys_wait_sema = 0x44;
constexpr std::int32_t sys_poll_sema = 0x45;

struct State {
    Machine* machine = nullptr;
    std::vector<std::string> log;
    std::vector<ThreadStatus> statuses;
    std::int32_t ids[4]{};
    std::int32_t results[4]{};
};
State g;

EeKernel& kernel() {
    return dynamic_cast<EeKernel&>(*g.machine->scheduler());
}

void prepare(TestMachine& m) {
    g = State{};
    g.machine = m.machine.get();
}

// A dormant thread at k_high, priority `priority`, with a stack.
std::int32_t create(TestMachine& m, std::int32_t priority) {
    m.set_word(k_param + 0x04, k_high);
    m.set_word(k_param + 0x08, 0x00024000u);
    m.set_word(k_param + 0x0C, 0x800u);
    m.set_word(k_param + 0x10, 0);
    m.set_word(k_param + 0x14, static_cast<std::uint32_t>(priority));
    return m.call(sys_create_thread, {k_param});
}

StopInfo stop_of(const SliceResult& r) {
    REQUIRE(r.stopped);
    return *r.stop;
}

} // namespace

TEST_CASE("kernel threads: CreateThread and CreateSema decode the EE parameter layouts",
          "[kernel][threads]") {
    HostProgram p;
    TestMachine m(p.table());
    m.boot(k_main);
    // status, entry, stack, stack_size, gp, initial_priority, current_priority, attr, option
    const std::uint32_t thread_param[9] = {0x11111111u, k_high, 0x00018000u, 0x1000u,    0x00123400u,
                                           37u,         99u,    0xABCDEF01u, 0x10203040u};
    for (std::uint32_t i = 0; i < 9; ++i) {
        m.set_word(k_param + 4 * i, thread_param[i]);
    }
    const std::int32_t id = m.call(sys_create_thread, {k_param});
    REQUIRE(id >= 2);
    const GuestThread* t = m.kernel().thread(id);
    REQUIRE(t != nullptr);
    CHECK(t->entry == k_high);        // 0x04
    CHECK(t->stack == 0x00018000u);   // 0x08
    CHECK(t->stack_size == 0x1000u);  // 0x0C
    CHECK(t->gp == 0x00123400u);      // 0x10
    CHECK(t->initial_priority == 37); // 0x14
    CHECK(t->current_priority == 37); // starts at the initial priority, not the status-only field
    CHECK(t->attr == 0xABCDEF01u);    // 0x1C
    CHECK(t->option == 0x10203040u);  // 0x20
    CHECK(t->status == ThreadStatus::dormant);
    // The whole descriptor must lie in guest memory.
    CHECK(m.call(sys_create_thread, {0x02000000u - kabi::thread_param::size + 4}) == kabi::error);

    // count, max_count, init_count, wait_threads, attr, option
    const std::uint32_t sema_param[6] = {91u, 7u, 3u, 82u, 0x55667788u, 0x99AABBCCu};
    for (std::uint32_t i = 0; i < 6; ++i) {
        m.set_word(k_param + 4 * i, sema_param[i]);
    }
    const std::int32_t sema = m.call(sys_create_sema, {k_param});
    REQUIRE(sema > 0);
    const Semaphore* s = m.kernel().semaphore(sema);
    REQUIRE(s != nullptr);
    CHECK(s->count == 3); // init_count at 0x08, not the status count at 0x00
    CHECK(s->max_count == 7);
    CHECK(s->attr == 0x55667788u);
    CHECK(s->option == 0x99AABBCCu);
}

TEST_CASE("kernel threads: lifecycle, nested suspend and wakeup counts", "[kernel][threads]") {
    HostProgram p;
    TestMachine m(p.table());
    m.boot(k_main);
    const std::int32_t id = create(m, 20);
    REQUIRE(id >= 2);
    const auto u = static_cast<std::uint32_t>(id);
    const GuestThread* t = m.kernel().thread(id);
    CHECK(m.call(sys_start_thread, {u, 0xCAFEu}) == kabi::ok);
    CHECK(t->status == ThreadStatus::ready);
    CHECK(m.call(sys_suspend_thread, {u}) == kabi::ok);
    CHECK(m.call(sys_suspend_thread, {u}) == kabi::ok); // nested
    CHECK(t->status == ThreadStatus::suspended);
    CHECK(t->suspend_count == 2);
    CHECK(m.call(sys_resume_thread, {u}) == kabi::ok); // only the count drops
    CHECK(t->status == ThreadStatus::suspended);
    CHECK(t->suspend_count == 1);
    CHECK(m.call(sys_resume_thread, {u}) == kabi::ok);
    CHECK(t->status == ThreadStatus::ready);
    CHECK(t->suspend_count == 0);
    CHECK(m.call(sys_resume_thread, {u}) == kabi::not_suspended);

    CHECK(m.call(sys_wakeup_thread, {u}) == kabi::ok); // not sleeping: the wakeups accumulate
    CHECK(m.call(sys_wakeup_thread, {u}) == kabi::ok);
    CHECK(m.call(sys_cancel_wakeup, {u}) == 2);
    CHECK(m.call(sys_cancel_wakeup, {u}) == 0);

    CHECK(m.call(sys_terminate_thread, {u}) == kabi::ok);
    CHECK(t->status == ThreadStatus::dormant);
    CHECK(m.call(sys_terminate_thread, {u}) == kabi::dormant);
    CHECK(m.call(sys_delete_thread, {u}) == kabi::ok);
    CHECK(m.kernel().thread(id) == nullptr);
}

TEST_CASE("kernel threads: a sleeping thread goes WAIT-SUSPEND, then SUSPEND, then READY",
          "[kernel][threads]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        const auto h = guest_create_thread(rdram, ctx, rt, k_param, k_helper, 0x20000u, 0x800u, 30);
        guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(h), 0});
        g.results[0] = guest_syscall(rdram, ctx, rt, sys_sleep_thread);
        g.statuses.push_back(kernel().thread(1)->status); // running again
        guest_request_stop(*g.machine);
    });
    p.set(k_helper, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.statuses.push_back(kernel().thread(1)->status); // waiting (sleep)
        guest_syscall(rdram, ctx, rt, sys_suspend_thread, {1});
        g.statuses.push_back(kernel().thread(1)->status); // waiting-suspended
        guest_syscall(rdram, ctx, rt, sys_wakeup_thread, {1});
        g.statuses.push_back(kernel().thread(1)->status);      // suspended: the wait completed
        guest_syscall(rdram, ctx, rt, sys_resume_thread, {1}); // ready, and priority 0 preempts us here
        guest_return(ctx);
    });
    TestMachine m(p.table());
    prepare(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.results[0] == kabi::ok);
    CHECK(g.statuses == std::vector<ThreadStatus>{ThreadStatus::waiting, ThreadStatus::waiting_suspended,
                                                  ThreadStatus::suspended, ThreadStatus::running});
}

TEST_CASE("kernel threads: invalid ids give the documented kernel errors", "[kernel][threads]") {
    HostProgram p;
    TestMachine m(p.table());
    m.boot(k_main);
    CHECK(m.call(sys_create_thread, {0}) == kabi::error);
    CHECK(m.call(sys_delete_thread, {0}) == kabi::illegal_thread_id);
    CHECK(m.call(sys_delete_thread, {1}) == kabi::illegal_thread_id); // the boot thread
    CHECK(m.call(sys_start_thread, {0x7FFF}) == kabi::unknown_thread_id);
    CHECK(m.call(sys_wakeup_thread, {0x7FFF}) == kabi::unknown_thread_id);
    CHECK(m.call(sys_poll_sema, {0x7FFF}) == kabi::unknown_semaphore_id);
    CHECK(m.call(sys_resume_thread, {0}) == kabi::illegal_thread_id);
    CHECK(m.call(sys_refer_thread_status, {0x7FFF, k_status}) == kabi::unknown_thread_id);
    CHECK(m.call(sys_change_priority, {1, 0}) == kabi::illegal_priority);
    CHECK(m.call(sys_change_priority, {1, 128}) == kabi::illegal_priority);
    CHECK(create(m, 0) == kabi::illegal_priority); // priority 0 belongs to the boot thread
    const std::int32_t id = create(m, 20);
    CHECK(m.call(sys_delete_thread, {static_cast<std::uint32_t>(id)}) == kabi::ok);
    CHECK(m.call(sys_start_thread, {static_cast<std::uint32_t>(id), 0}) == kabi::unknown_thread_id);
}

TEST_CASE("kernel threads: ReferThreadStatus reports the status block, or the status alone",
          "[kernel][threads]") {
    HostProgram p;
    TestMachine m(p.table());
    m.boot(k_main);
    const std::int32_t id = create(m, 20);
    const auto u = static_cast<std::uint32_t>(id);
    CHECK(m.call(sys_refer_thread_status, {u, 0}) == kabi::status_dormant);
    CHECK(m.call(sys_start_thread, {u, 0}) == kabi::ok);
    CHECK(m.call(sys_refer_thread_status, {u, k_status}) == kabi::ok);
    CHECK(m.word(k_status + kabi::thread_param::status) == static_cast<std::uint32_t>(kabi::status_ready));
    CHECK(m.word(k_status + kabi::thread_param::entry) == k_high);
    CHECK(m.word(k_status + kabi::thread_param::stack) == 0x00024000u);
    CHECK(m.word(k_status + kabi::thread_param::stack_size) == 0x800u);
    CHECK(m.word(k_status + kabi::thread_param::initial_priority) == 20u);
    CHECK(m.word(k_status + kabi::thread_param::current_priority) == 20u);
    CHECK(m.word(k_status + kabi::thread_param::wait_type) == kabi::wait_none);
}

TEST_CASE("kernel threads: ExitDeleteThread removes the record; a started thread runs from its entry",
          "[kernel][threads]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.ids[0] = guest_create_thread(rdram, ctx, rt, k_param, k_helper, 0x20000u, 0x800u, 10);
        g.results[0] = guest_syscall(rdram, ctx, rt, sys_get_thread_id);
        guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(g.ids[0]), 0x1234u});
        // The helper is lower priority: it runs once we sleep, and wakes us before it exits.
        guest_syscall(rdram, ctx, rt, sys_sleep_thread);
        g.results[2] = kernel().thread(g.ids[0]) == nullptr ? 1 : 0;
        guest_request_stop(*g.machine);
    });
    p.set(k_helper, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.results[1] = static_cast<std::int32_t>(dq8::rt::gpr_u32(*ctx, 4)); // StartThread's argument in $a0
        g.results[3] = guest_syscall(rdram, ctx, rt, sys_get_thread_id);
        guest_syscall(rdram, ctx, rt, sys_wakeup_thread, {1}); // the boot thread preempts us here
        guest_syscall(rdram, ctx, rt, sys_exit_delete_thread);
    });
    TestMachine m(p.table());
    prepare(m);
    // The waking thread preempts the helper before its ExitDeleteThread; the run then continues until the
    // helper exits. Run in two slices to see both.
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.results[0] == 1);
    CHECK(g.results[1] == 0x1234);
    CHECK(g.results[3] == g.ids[0]);
    CHECK(g.results[2] == 0); // still there: it was preempted before ExitDeleteThread
    const auto started = std::count_if(m.events.begin(), m.events.end(), [](const MachineEvent& e) {
        return e.type == "thread" && e.fields.size() >= 2 &&
               e.fields[0].second == MachineEvent::Value(std::string("start"));
    });
    CHECK(started == 1);
}

TEST_CASE("kernel threads: a thread whose entry returns exits, and ExitDeleteThread forgets it",
          "[kernel][threads]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.ids[0] = guest_create_thread(rdram, ctx, rt, k_param, k_helper, 0x20000u, 0x800u, 10);
        g.ids[1] = guest_create_thread(rdram, ctx, rt, k_param, k_high, 0x21000u, 0x800u, 10);
        g.results[0] = guest_create_sema(rdram, ctx, rt, k_param + 0x100, 0, 2);
        guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(g.ids[0]), 0});
        guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(g.ids[1]), 0});
        guest_syscall(rdram, ctx, rt, sys_wait_sema, {static_cast<std::uint32_t>(g.results[0])});
        guest_syscall(rdram, ctx, rt, sys_wait_sema, {static_cast<std::uint32_t>(g.results[0])});
        g.statuses.push_back(kernel().thread(g.ids[0])->status);     // its entry returned: dormant
        g.results[1] = kernel().thread(g.ids[1]) == nullptr ? 1 : 0; // preempted before ExitDeleteThread
        // Drop below it: it runs its ExitDeleteThread, and its record is gone when we come back.
        guest_syscall(rdram, ctx, rt, sys_change_priority, {0, 20});
        g.results[2] = kernel().thread(g.ids[1]) == nullptr ? 1 : 0;
        guest_request_stop(*g.machine);
    });
    p.set(k_helper, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        guest_syscall(rdram, ctx, rt, 0x42, {static_cast<std::uint32_t>(g.results[0])}); // SignalSema
        guest_return(ctx); // $ra is 0: the entry returning exits the thread
    });
    p.set(k_high, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        guest_syscall(rdram, ctx, rt, 0x42, {static_cast<std::uint32_t>(g.results[0])});
        guest_syscall(rdram, ctx, rt, sys_exit_delete_thread);
    });
    TestMachine m(p.table());
    prepare(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.statuses == std::vector<ThreadStatus>{ThreadStatus::dormant});
    CHECK(g.results[1] == 0);
    CHECK(g.results[2] == 1);
}

TEST_CASE("kernel threads: raising a ready thread above the running one preempts it", "[kernel][threads]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        // Drop to priority 50, start a priority-40 helper (it preempts), and let it raise itself above us.
        std::int32_t old = 0;
        (void)old;
        g.ids[0] = guest_create_thread(rdram, ctx, rt, k_param, k_helper, 0x20000u, 0x800u, 60);
        g.results[0] = guest_syscall(rdram, ctx, rt, sys_change_priority, {0, 50});
        guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(g.ids[0]), 0});
        g.log.push_back("main before");
        guest_syscall(rdram, ctx, rt, sys_change_priority, {static_cast<std::uint32_t>(g.ids[0]), 10});
        g.log.push_back("main after");
        guest_request_stop(*g.machine);
    });
    p.set(k_helper, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
        g.log.push_back("helper");
        guest_return(ctx);
    });
    TestMachine m(p.table());
    prepare(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.results[0] == kabi::ok);
    CHECK(g.log == std::vector<std::string>{"main before", "helper", "main after"});
}

TEST_CASE("kernel threads: StartThread of a thread without a stack stops", "[kernel][threads]") {
    HostProgram p;
    TestMachine m(p.table());
    m.boot(k_main);
    m.set_word(k_param + 0x04, k_high);
    m.set_word(k_param + 0x08, 0);
    m.set_word(k_param + 0x0C, 0);
    m.set_word(k_param + 0x14, 20);
    const std::int32_t id = m.call(sys_create_thread, {k_param});
    REQUIRE(id >= 2);
    try {
        m.call(sys_start_thread, {static_cast<std::uint32_t>(id), 0});
        FAIL("StartThread returned");
    } catch (const GuestStop& stop) {
        CHECK(stop.info().message.find("without a stack") != std::string::npos);
    }
}
