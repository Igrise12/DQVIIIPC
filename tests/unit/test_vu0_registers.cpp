// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// VU0's registers across threads and interrupt handlers (M4 session D): a handler starts with a copy of the
// interrupted thread's VU0 block; with --vu0-registers shared one register file follows the CPU from thread
// to thread and keeps a handler's changes; per-thread (the default) keeps each thread's own.
#include "runtime/ee/context.h"
#include "runtime/machine/ee_devices.h"
#include "runtime/machine/interrupts.h"
#include "runtime/machine/machine.h"
#include "runtime/vu/vu_units.h"
#include "support/ee_rig.h"
#include "support/machine_harness.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <xmmintrin.h>

namespace {

using namespace dq8::rt;
using dq8::test::guest_create_thread;
using dq8::test::guest_request_stop;
using dq8::test::guest_return;
using dq8::test::guest_syscall;
using dq8::test::HostProgram;
using dq8::test::TestMachine;

constexpr std::uint32_t k_main = 0x00300000u;
constexpr std::uint32_t k_helper = 0x00300100u;
constexpr std::uint32_t k_handler = 0x00300200u;
constexpr std::uint32_t k_param = 0x00001000u;

constexpr std::int32_t sys_add_intc = 0x10;
constexpr std::int32_t sys_enable_intc = 0x14;
constexpr std::int32_t sys_start_thread = 0x22;
constexpr std::int32_t sys_sleep_thread = 0x32;
constexpr std::int32_t sys_wakeup_thread = 0x33;

float lane(__m128 v, int i) {
    float out[4];
    _mm_storeu_ps(out, v);
    return out[i];
}

struct State {
    Machine* machine = nullptr;
    float handler_saw = 0;
    std::uint16_t handler_vi = 0;
    float main_after_handler = 0;
    float helper_saw = -1;
    float main_after_helper = 0;
};
State g;

TestMachine machine_for(const HostProgram& p, bool shared) {
    MachineConfig config;
    config.vu0_shared = shared;
    return TestMachine(p.table(), config);
}

} // namespace

TEST_CASE("vu0 registers: copy_vu0_block copies the VU0 block and nothing else", "[vu0][kernel]") {
    R5900Context a;
    R5900Context b;
    a.vu0_vf[7] = _mm_set_ps(4.0f, 3.0f, 2.0f, 1.0f);
    a.vi[9] = 0x1234;
    a.vu0_q = 0.5f;
    a.vu0_clip_flags = 0xABCDEF;
    a.vu0_cf[2] = 9.0f;
    a.cop2_ccr[20] = 77;
    a.pc = 0x1000;
    a.cop0_status = 0;
    a.f[3] = 6.0f;
    copy_vu0_block(a, b);
    CHECK(lane(b.vu0_vf[7], 0) == 1.0f);
    CHECK(lane(b.vu0_vf[7], 3) == 4.0f);
    CHECK(b.vi[9] == 0x1234);
    CHECK(b.vu0_q == 0.5f);
    CHECK(b.vu0_clip_flags == 0xABCDEFu);
    CHECK(b.vu0_cf[2] == 9.0f);
    CHECK(b.cop2_ccr[20] == 77u);
    CHECK(b.pc == 0u);
    CHECK(b.cop0_status == R5900Context().cop0_status);
    CHECK(b.f[3] == 0.0f);
}

namespace {

// main: sets VF5.x and VI3, waits for a timer interrupt; the handler records what it sees and changes VF5.x;
// main records its VF5.x afterwards. Then main starts a helper thread and sleeps; the helper records VF5.x,
// sets VF6.x and wakes main, which records VF6.x.
HostProgram vu0_program() {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        ctx->vu0_vf[5] = _mm_set_ps(0.0f, 0.0f, 0.0f, 1.5f);
        ctx->vi[3] = 7;
        guest_syscall(rdram, ctx, rt, sys_add_intc, {9, k_handler, 0, 0});
        guest_syscall(rdram, ctx, rt, sys_enable_intc, {9});
        while (g.handler_saw == 0) {
            rt->eeCheckpointDue();
        }
        g.main_after_handler = lane(ctx->vu0_vf[5], 0);
        const auto h = guest_create_thread(rdram, ctx, rt, k_param, k_helper, 0x20000u, 0x800u, 30);
        guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(h), 0});
        guest_syscall(rdram, ctx, rt, sys_sleep_thread);
        g.main_after_helper = lane(ctx->vu0_vf[6], 0);
        guest_request_stop(*g.machine);
    });
    p.set(k_handler, [](std::uint8_t* /*rdram*/, R5900Context* ctx, PS2Runtime* /*rt*/) {
        g.handler_saw = lane(ctx->vu0_vf[5], 0);
        g.handler_vi = ctx->vi[3];
        ctx->vu0_vf[5] = _mm_set_ps(0.0f, 0.0f, 0.0f, 8.0f);
        guest_return(ctx);
    });
    p.set(k_helper, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.helper_saw = lane(ctx->vu0_vf[5], 0);
        ctx->vu0_vf[6] = _mm_set_ps(0.0f, 0.0f, 0.0f, 3.0f);
        guest_syscall(rdram, ctx, rt, sys_wakeup_thread, {1});
        guest_return(ctx);
    });
    return p;
}

void run_vu0_program(bool shared) {
    const HostProgram p = vu0_program();
    TestMachine m = machine_for(p, shared);
    g = State{};
    g.machine = m.machine.get();
    m.boot(k_main);
    m.machine->events().schedule(5000, "test-timer0",
                                 [&m](Cycles) { m.machine->interrupts().raise_intc(IntcLine::timer0); });
    const SliceResult r = m.run();
    REQUIRE(r.stopped);
    CHECK(r.stop->reason == StopReason::requested);
}

} // namespace

TEST_CASE("vu0 registers: per-thread, a handler sees the interrupted thread's and its changes stay its own",
          "[vu0][kernel]") {
    run_vu0_program(false);
    CHECK(g.handler_saw == 1.5f); // a copy of the interrupted thread's block
    CHECK(g.handler_vi == 7);
    CHECK(g.main_after_handler == 1.5f); // the handler's change was dropped
    CHECK(g.helper_saw == 0.0f);         // another thread has its own registers
    CHECK(g.main_after_helper == 0.0f);
}

TEST_CASE("vu0 registers: shared, one register file follows the CPU and keeps a handler's changes",
          "[vu0][kernel]") {
    run_vu0_program(true);
    CHECK(g.handler_saw == 1.5f);
    CHECK(g.main_after_handler == 8.0f);
    CHECK(g.helper_saw == 8.0f);
    CHECK(g.main_after_helper == 3.0f);
}

namespace {

constexpr std::uint32_t lower_nop = 0x8000033Cu;
constexpr std::uint32_t upper_nop = 0x000002FFu;
constexpr std::uint32_t e_bit = 1u << 30;
// Upper VADD.xyzw fd, fs, ft.
constexpr std::uint32_t vadd(unsigned fd, unsigned fs, unsigned ft) {
    return (0xFu << 21) | (ft << 16) | (fs << 11) | (fd << 6) | 0x28u;
}

struct DeviceState {
    VuUnits* vu = nullptr;
    float main_vf3 = 0;
};
DeviceState d;

// A real machine with the EE devices (so the VU device sees the kernel) running `p`.
struct DeviceMachine {
    explicit DeviceMachine(const HostProgram& p) : m(p.table()) {
        devices = make_ee_devices(EeDevicesConfig{&sink});
        for (auto& dev : devices) {
            m.machine->attach(*dev);
            if (auto* vu = dynamic_cast<VuUnits*>(dev.get())) {
                d.vu = vu;
            }
        }
        REQUIRE(d.vu != nullptr);
    }
    dq8::test::CaptureSink sink;
    TestMachine m;
    std::vector<std::unique_ptr<IDevice>> devices;
};

// VADD vf3, vf1, vf2 at 0x40; written by the guest code (boot resets micro memory).
void load_vadd() {
    std::vector<std::uint8_t> code;
    for (const std::uint32_t w : {lower_nop, vadd(3, 1, 2) | e_bit, lower_nop, upper_nop}) {
        dq8::test::put32(code, w);
    }
    d.vu->write_micro(0, 0x40, code);
}

} // namespace

TEST_CASE("vu0 registers: VIF0's MSCAL runs on the VU0 registers the running thread sees", "[vu0][kernel]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* /*rdram*/, R5900Context* ctx, PS2Runtime* /*rt*/) {
        load_vadd();
        ctx->vu0_vf[1] = _mm_set_ps(4.0f, 3.0f, 2.0f, 1.0f);
        ctx->vu0_vf[2] = _mm_set_ps(40.0f, 30.0f, 20.0f, 10.0f);
        d.vu->start(0, 0x40, 0, 0); // what VIF0 does for an MSCAL while this thread holds the CPU
        d.main_vf3 = lane(ctx->vu0_vf[3], 1);
        guest_request_stop(*g.machine);
    });
    d = DeviceState{};
    DeviceMachine dm(p);
    g = State{};
    g.machine = dm.m.machine.get();
    const SliceResult r = dm.m.run_from(k_main);
    REQUIRE(r.stopped);
    INFO(r.stop->message);
    CHECK(r.stop->reason == StopReason::requested);
    CHECK(d.main_vf3 == 22.0f);
}

TEST_CASE("vu0 registers: the first VU0 program of another thread is reported", "[vu0][kernel]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        load_vadd();
        rt->executeVU0Microprogram(rdram, ctx, 0x40);
        rt->executeVU0Microprogram(rdram, ctx, 0x40);
        const auto h = guest_create_thread(rdram, ctx, rt, k_param, k_helper, 0x20000u, 0x800u, 30);
        guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(h), 0});
        guest_syscall(rdram, ctx, rt, sys_sleep_thread);
        rt->executeVU0Microprogram(rdram, ctx, 0x40);
        guest_request_stop(*g.machine);
    });
    p.set(k_helper, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        rt->executeVU0Microprogram(rdram, ctx, 0x40);
        rt->executeVU0Microprogram(rdram, ctx, 0x40);
        guest_syscall(rdram, ctx, rt, sys_wakeup_thread, {1});
        guest_return(ctx);
    });
    d = DeviceState{};
    DeviceMachine dm(p);
    g = State{};
    g.machine = dm.m.machine.get();
    const SliceResult r = dm.m.run_from(k_main);
    REQUIRE(r.stopped);
    INFO(r.stop->message);
    CHECK(r.stop->reason == StopReason::requested);
    std::vector<std::string> seen;
    for (const MachineEvent& e : dm.m.events) {
        if (e.type == "vu0_cross_thread") {
            seen.push_back(std::to_string(std::get<std::int64_t>(e.fields[1].second)) + "->" +
                           std::to_string(std::get<std::int64_t>(e.fields[0].second)));
        }
    }
    CHECK(seen == std::vector<std::string>{"1->2", "2->1"});
    CHECK(d.vu->stats(0).programs == 5);
}
