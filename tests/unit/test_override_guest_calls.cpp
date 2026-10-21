// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Native override code calling into the guest (OverrideCall::call_guest, scratch, kernel_call; M4 day 0): the
// sceMpeg HLE calls the movie player's callbacks this way. The callee runs as a nested call on the thread's
// fiber, may block and resume, and may be terminated with its thread.
#include "runtime/ee/context.h"
#include "runtime/ee/overrides.h"
#include "support/machine_harness.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

using namespace dq8::rt;
using dq8::test::guest_create_sema;
using dq8::test::guest_create_thread;
using dq8::test::guest_named;
using dq8::test::guest_request_stop;
using dq8::test::guest_return;
using dq8::test::guest_syscall;
using dq8::test::HostProgram;
using dq8::test::peek;
using dq8::test::TestMachine;

namespace {

constexpr std::uint32_t k_main = 0x00300000u;
constexpr std::uint32_t k_over = 0x00300100u;   // the overridden function
constexpr std::uint32_t k_callee = 0x00300200u; // what the override calls
constexpr std::uint32_t k_t = 0x00300300u;      // a thread that calls k_over
constexpr std::uint32_t k_s = 0x00300400u;      // a thread that signals or terminates
constexpr std::uint32_t k_param = 0x00001000u;

constexpr std::int32_t sys_start_thread = 0x22;
constexpr std::int32_t sys_exit_thread = 0x23;
constexpr std::int32_t sys_signal_sema = 0x42;
constexpr std::int32_t sys_wait_sema = 0x44;

struct State {
    Machine* machine = nullptr;
    Dispatcher* dispatcher = nullptr;
    std::vector<int> trace;
    std::uint64_t value = 0;
    std::uint64_t thread_id = 0;
    std::int32_t main_id = 0;
    std::uint32_t block = 0;
    std::uint32_t caller_sp = 0;
    std::uint32_t callee_sp = 0;
    std::int32_t sema = 0;
    std::int32_t thread = 0;
    bool override_done = false;
};
State g;

// Calls `target` the way generated code's jal does: through the dispatch table (where the override sits),
// with $ra at the instruction after the delay slot.
void jal(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt, std::uint32_t from, std::uint32_t target) {
    set_gpr_lo(*ctx, 31, from + 8);
    ctx->pc = target;
    g.dispatcher->lookup(target)(rdram, ctx, rt);
}

void install(TestMachine& m, OverrideFn fn) {
    OverrideSpec spec;
    spec.unit = "test";
    spec.address = k_over;
    spec.kind = OverrideKind::replace;
    spec.name = "test-call-guest";
    spec.fn = std::move(fn);
    m.dispatcher.set_machine(m.machine.get());
    m.dispatcher.install_override(std::move(spec));
}

TestMachine& start(TestMachine& m) {
    g = State{};
    g.machine = m.machine.get();
    g.dispatcher = &m.dispatcher;
    return m;
}

void original(std::uint8_t* /*rdram*/, R5900Context* ctx, PS2Runtime* /*rt*/) {
    guest_return(ctx); // never runs: the override replaces it
}

} // namespace

TEST_CASE(
    "override guest calls: a callee gets its arguments, a stack below the scratch block, and returns $v0",
    "[overrides][guest_calls]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.caller_sp = gpr_u32(*ctx, 29);
        g.main_id = guest_named(rdram, ctx, rt, "GetThreadId");
        jal(rdram, ctx, rt, k_main, k_over);
        g.trace.push_back(ctx->pc == k_main + 8 ? 1 : -1); // the override returned to its caller
        guest_request_stop(*g.machine);
    });
    p.set(k_over, &original);
    p.set(k_callee, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* /*rt*/) {
        g.callee_sp = gpr_u32(*ctx, 29);
        set_gpr_lo(*ctx, 2, peek(rdram, gpr_u32(*ctx, 4)) + gpr_u32(*ctx, 5) + gpr_u32(*ctx, 8));
        guest_return(ctx);
    });
    TestMachine m(p.table());
    start(m);
    install(m, [](OverrideCall& c) {
        g.block = c.scratch(16);
        c.machine->memory().store32(g.block, 0x1230);
        g.value = c.call_guest(k_callee, {g.block, 5, 0, 0, 4}); // $a0, $a1, $a2, $a3, $t0
        g.thread_id = c.kernel_call("GetThreadId");
        g.override_done = true;
        c.return_to_caller();
    });
    const SliceResult r = m.run_from(k_main);
    REQUIRE(r.stopped);
    CHECK(r.stop->reason == StopReason::requested);
    CHECK(g.override_done);
    CHECK(g.value == 0x1239);
    CHECK(g.trace == std::vector<int>{1});
    CHECK(g.block < g.caller_sp);
    CHECK(g.block % 16 == 0);
    CHECK(g.callee_sp < g.block); // the callee's frames cannot overwrite the block
    CHECK(g.thread_id == static_cast<std::uint64_t>(g.main_id)); // the kernel call ran on the calling thread
    CHECK(g.main_id > 0);
}

TEST_CASE("override guest calls: a callee that blocks resumes and returns into the override",
          "[overrides][guest_calls]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.sema = guest_create_sema(rdram, ctx, rt, k_param + 0x100, 0, 1);
        const auto t = guest_create_thread(rdram, ctx, rt, k_param, k_t, 0x20000u, 0x800u, 5);
        const auto s = guest_create_thread(rdram, ctx, rt, k_param, k_s, 0x22000u, 0x800u, 20);
        for (const auto id : {t, s}) {
            guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(id), 0});
        }
        guest_syscall(rdram, ctx, rt, sys_exit_thread);
    });
    p.set(k_t, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(10);
        jal(rdram, ctx, rt, k_t, k_over);
        g.trace.push_back(11);
        guest_request_stop(*g.machine);
    });
    p.set(k_over, &original);
    p.set(k_callee, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(12);
        guest_syscall(rdram, ctx, rt, sys_wait_sema, {static_cast<std::uint32_t>(g.sema)});
        g.trace.push_back(13);
        set_gpr_lo(*ctx, 2, 7);
        guest_return(ctx);
    });
    p.set(k_s, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(20);
        guest_syscall(rdram, ctx, rt, sys_signal_sema, {static_cast<std::uint32_t>(g.sema)});
        g.trace.push_back(21);
        guest_return(ctx);
    });
    TestMachine m(p.table());
    start(m);
    install(m, [](OverrideCall& c) {
        g.value = c.call_guest(k_callee);
        g.override_done = true;
        c.return_to_caller();
    });
    const SliceResult r = m.run_from(k_main);
    REQUIRE(r.stopped);
    CHECK(r.stop->reason == StopReason::requested);
    // T blocks inside the callee; S signals (T, at higher priority, preempts at once), and T returns through
    // the override.
    CHECK(g.trace == std::vector<int>{10, 12, 20, 13, 11});
    CHECK(g.value == 7);
    CHECK(g.override_done);
}

TEST_CASE("override guest calls: a thread terminated inside the callee leaves the machine running",
          "[overrides][guest_calls]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.sema = guest_create_sema(rdram, ctx, rt, k_param + 0x100, 0, 1);
        g.thread = guest_create_thread(rdram, ctx, rt, k_param, k_t, 0x20000u, 0x800u, 5);
        const auto s = guest_create_thread(rdram, ctx, rt, k_param, k_s, 0x22000u, 0x800u, 20);
        for (const auto id : {g.thread, s}) {
            guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(id), 0});
        }
        guest_syscall(rdram, ctx, rt, sys_exit_thread);
    });
    p.set(k_t, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(10);
        jal(rdram, ctx, rt, k_t, k_over);
        g.trace.push_back(-1); // never: T is terminated inside the callee
        guest_return(ctx);
    });
    p.set(k_over, &original);
    p.set(k_callee, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(12);
        guest_syscall(rdram, ctx, rt, sys_wait_sema, {static_cast<std::uint32_t>(g.sema)});
        g.trace.push_back(-2); // never
        guest_return(ctx);
    });
    p.set(k_s, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(20);
        guest_named(rdram, ctx, rt, "TerminateThread", {static_cast<std::uint32_t>(g.thread)});
        guest_named(rdram, ctx, rt, "DeleteThread", {static_cast<std::uint32_t>(g.thread)});
        g.trace.push_back(21);
        guest_request_stop(*g.machine);
    });
    TestMachine m(p.table());
    start(m);
    install(m, [](OverrideCall& c) {
        g.value = c.call_guest(k_callee);
        g.override_done = true;
        c.return_to_caller();
    });
    const SliceResult r = m.run_from(k_main);
    REQUIRE(r.stopped);
    CHECK(r.stop->reason == StopReason::requested);
    CHECK(g.trace == std::vector<int>{10, 12, 20, 21});
    CHECK_FALSE(g.override_done);
}
