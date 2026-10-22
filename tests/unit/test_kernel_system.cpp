// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors: the system cases of
// ps2xTest/src/ps2_runtime_kernel_tests.cpp ("unsigned loads and ABI word writes extend independently", the
// heap parts of "setup heap and allocator primitives track end-of-heap" and "setup heap and thread invalid
// ids use documented kernel errors", "SetupThread exposes stable main stack metadata through
// ReferThreadStatus", "OSD config2 syscalls round-trip extended config", the three FindAddress cases, the
// four SetSyscall table and isolation cases, the five SetSyscall override cases, "Copy syscall (0x5A)
// performs a memory copy", "GetEntryAddress syscall (0x5B) returns handler from guest table"). Expected
// values are upstream's except one: an override that jumps to an address with no function stops the run here
// (upstream completes it silently). Upstream's allocator, libc-stub and exit-handler cases test library fakes
// this port does not have. The kernel-RAM, noop, exit-path, coverage and stale-return cases are ours.
#include "dq8/abi/ps2recomp/ps2_runtime_macros.h"
#include "runtime/kernel/kernel_abi.h"
#include "runtime/kernel/kernel_layout.h"
#include "runtime/kernel/kernel_services.h"
#include "support/machine_harness.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace dq8::rt;
using dq8::test::guest_create_thread;
using dq8::test::guest_request_stop;
using dq8::test::guest_return;
using dq8::test::guest_syscall;
using dq8::test::HostProgram;
using dq8::test::poke;
using dq8::test::TestMachine;

constexpr std::uint32_t k_main = 0x00300000u;
constexpr std::uint32_t k_handler = 0x00300100u;
constexpr std::uint32_t k_driver = 0x00300200u;
constexpr std::uint32_t k_param = 0x00001000u;
constexpr std::uint32_t k_status = 0x00001400u;

constexpr std::int32_t sys_start_thread = 0x22;
constexpr std::int32_t sys_refer_thread_status = 0x30;
constexpr std::int32_t sys_sleep_thread = 0x32;
constexpr std::int32_t sys_wakeup_thread = 0x33;
constexpr std::int32_t sys_setup_thread = 0x3C;
constexpr std::int32_t sys_setup_heap = 0x3D;
constexpr std::int32_t sys_end_of_heap = 0x3E;
constexpr std::int32_t sys_set_osd = 0x4A;
constexpr std::int32_t sys_get_osd = 0x4B;
constexpr std::int32_t sys_copy = 0x5A;
constexpr std::int32_t sys_get_entry_address = 0x5B;
constexpr std::int32_t sys_set_osd2 = 0x6E;
constexpr std::int32_t sys_get_osd2 = 0x6F;
constexpr std::int32_t sys_set_syscall = 0x74;
constexpr std::int32_t sys_get_memory_size = 0x7F;
constexpr std::int32_t sys_find_address = 0x83;

constexpr std::uint64_t k_upper64 = 0x1122334455667788ull;

struct State {
    Machine* machine = nullptr;
    OverlayManager* overlays = nullptr;
    std::vector<int> trace;
    std::uint32_t syscall = 0;
    std::uint32_t args[3]{};
    std::uint32_t results[4]{};
    bool after_call = false;
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

// A boot thread that makes syscall g.syscall with $a0..$a2 = g.args, keeps $v0, and stops.
void override_entry(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
    g.results[0] = static_cast<std::uint32_t>(guest_syscall(
        rdram, ctx, rt, static_cast<std::int32_t>(g.syscall), {g.args[0], g.args[1], g.args[2]}));
    guest_request_stop(*g.machine);
}

// Runs override_entry with `handler` installed for g.syscall through SetSyscall.
SliceResult run_override(TestMachine& m, HostProgram& p, dq8_guest_fn handler) {
    p.set(k_main, &override_entry);
    p.set(k_handler, handler);
    m.boot(k_main);
    CHECK(m.call(sys_set_syscall, {g.syscall, k_handler}) == kabi::ok);
    return m.run();
}

} // namespace

TEST_CASE("kernel system: unsigned loads and ABI word writes extend independently", "[kernel][system]") {
    R5900Context ctx;
    ctx.r[2] = _mm_set_epi64x(static_cast<std::int64_t>(k_upper64), 0);
    SET_GPR_ZE32(&ctx, 2, 0x80000000u);
    CHECK(gpr_lo(ctx, 2) == 0x0000000080000000ull); // LWU/LHU/LBU zero-extend
    CHECK(gpr_hi(ctx, 2) == k_upper64);
    SET_GPR_U32(&ctx, 2, 0x80000000u);
    CHECK(gpr_lo(ctx, 2) == 0xFFFFFFFF80000000ull); // the EE's 32-bit results sign-extend
    CHECK(gpr_hi(ctx, 2) == k_upper64);
}

TEST_CASE("kernel system: SetupHeap and EndOfHeap", "[kernel][system]") {
    HostProgram p;
    TestMachine m(p.table());
    m.boot(k_main);
    CHECK(static_cast<std::uint32_t>(m.call(sys_setup_heap, {0x00180010u, 0x1000u})) == 0x00180010u);
    CHECK(static_cast<std::uint32_t>(m.call(sys_end_of_heap)) == 0x00181010u);
    // An out-of-range base wraps into RAM; the heap end stays at or above it.
    const auto base = static_cast<std::uint32_t>(m.call(sys_setup_heap, {0xFFFFFFFFu, 0}));
    CHECK(base < 0x02000000u);
    CHECK(static_cast<std::uint32_t>(m.call(sys_end_of_heap)) >= base);
    CHECK(static_cast<std::uint32_t>(m.call(sys_get_memory_size)) == 0x02000000u);
}

TEST_CASE("kernel system: SetupThread's stack and the heap that ends at it", "[kernel][system]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        // As the boot path calls it (M3-boots.md): gp, the stack's lowest address, its size, args.
        g.results[0] = static_cast<std::uint32_t>(guest_syscall(
            rdram, ctx, rt, sys_setup_thread, {0x003D9770u, 0x01F40000u, 0xC0000u, 0x0041F680u}));
        g.results[1] = static_cast<std::uint32_t>(
            guest_syscall(rdram, ctx, rt, sys_setup_heap, {0x00489A80u, 0xFFFFFFFFu}));
        g.results[2] = static_cast<std::uint32_t>(guest_syscall(rdram, ctx, rt, sys_end_of_heap));
        g.results[3] = gpr_u32(*ctx, 28);
        guest_request_stop(*g.machine);
    });
    TestMachine m(p.table());
    prepare(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.results[0] == 0x02000000u); // stack + size: the top of RAM
    CHECK(g.results[1] == 0x00489A80u);
    CHECK(g.results[2] == 0x01F40000u); // -1: the heap runs up to the main stack
    CHECK(g.results[3] == 0x003D9770u); // $gp set from the first argument
    CHECK(m.kernel().main_stack_base() == 0x01F40000u);
}

TEST_CASE("kernel system: SetupThread exposes stable main-stack metadata through ReferThreadStatus",
          "[kernel][system]") {
    constexpr std::uint32_t loader_sp = 0x02000000u - 0x10u;
    constexpr std::uint32_t stack_size = 0x00020000u;
    constexpr std::uint32_t expected_stack = 0x02000000u - stack_size;
    constexpr std::uint32_t main_gp = 0x0036A7F0u;
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        set_gpr_lo(*ctx, 29, loader_sp);
        g.results[0] = static_cast<std::uint32_t>(
            guest_syscall(rdram, ctx, rt, sys_setup_thread, {main_gp, 0xFFFFFFFFu, stack_size}));
        set_gpr_lo(*ctx, 29, expected_stack - 0x80u); // nested frames moved $sp since
        g.results[1] =
            static_cast<std::uint32_t>(guest_syscall(rdram, ctx, rt, sys_refer_thread_status, {0, k_status}));
        guest_request_stop(*g.machine);
    });
    TestMachine m(p.table());
    prepare(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK(g.results[0] == expected_stack); // automatic main stack below the top of RAM
    CHECK(static_cast<std::int32_t>(g.results[1]) == kabi::ok);
    CHECK(m.word(k_status + kabi::thread_param::stack) == expected_stack);
    CHECK(m.word(k_status + kabi::thread_param::stack_size) == stack_size);
    CHECK(m.word(k_status + kabi::thread_param::gp) == main_gp);
    CHECK(m.word(k_status + kabi::thread_param::status) == static_cast<std::uint32_t>(kabi::status_run));
}

TEST_CASE("kernel system: SetupThread always returns a 16-byte aligned stack pointer", "[kernel][system]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        set_gpr_lo(*ctx, 29, 0x0010FFF0u);
        g.results[0] = static_cast<std::uint32_t>(guest_syscall(rdram, ctx, rt, sys_setup_thread, {1, 0, 0}));
        g.results[1] = static_cast<std::uint32_t>(
            guest_syscall(rdram, ctx, rt, sys_setup_thread, {0, 0x00200007u, 0x101u}));
        guest_request_stop(*g.machine);
    });
    TestMachine m(p.table());
    prepare(m);
    CHECK(stop_of(m.run_from(k_main)).reason == StopReason::requested);
    CHECK((g.results[0] & 0xFu) == 0);
    CHECK((g.results[1] & 0xFu) == 0);
}

TEST_CASE("kernel system: the OSD configuration round-trips through both parameter forms",
          "[kernel][system]") {
    HostProgram p;
    TestMachine m(p.table());
    m.boot(k_main);
    // The defaults: English, OSD version 1, a non-Japanese console, SPDIF off.
    CHECK(m.call(sys_get_osd, {k_param}) == kabi::ok);
    CHECK(((m.word(k_param) >> 16) & 0x1Fu) == 1u);
    CHECK(((m.word(k_param) >> 13) & 0x7u) == 1u);
    constexpr std::uint32_t config1 = (1u << 0) | (1u << 4) | (1u << 13) | (1u << 16);
    constexpr std::uint32_t config2 = 0xABu | (0xB0u << 8) | (2u << 16) | (10u << 24);
    m.set_word(k_param, config1);
    CHECK(m.call(sys_set_osd, {k_param}) == kabi::ok);
    m.set_word(0x5000, config2);
    CHECK(m.call(sys_set_osd2, {0x5000, 4, 0}) == kabi::ok);
    m.set_word(0x5010, 0xFFFFFFFFu);
    CHECK(m.call(sys_get_osd2, {0x5010, 4, 0}) == kabi::ok);
    CHECK(m.word(0x5010) == config2);
    CHECK(((m.word(0x5010) >> 12) & 1u) == 1u); // daylight saving at bit 12
    CHECK(m.call(sys_get_osd, {0x5020}) == kabi::ok);
    CHECK(((m.word(0x5020) >> 13) & 0x7u) == 2u);   // version synced from Config2
    CHECK(((m.word(0x5020) >> 16) & 0x1Fu) == 10u); // language synced from Config2
    CHECK(m.call(sys_get_osd, {0x03000000u}) == kabi::error);
}

TEST_CASE("kernel system: FindAddress", "[kernel][system]") {
    HostProgram p;
    TestMachine m(p.table());
    m.boot(k_main);
    SECTION("finds the first matching word") {
        const std::uint32_t values[] = {0x11111111u, 0x11223344u, 0x55555555u, 0x89ABCDEFu};
        for (std::uint32_t i = 0; i < 4; ++i) {
            m.set_word(0x2000 + 4 * i, values[i]);
        }
        CHECK(static_cast<std::uint32_t>(m.call(sys_find_address, {0x2000, 0x2010, 0x11223344u})) == 0x2004u);
    }
    SECTION("matches kseg aliases and keeps the caller's segment") {
        m.set_word(0x3000, 0x00123456u);
        m.set_word(0x3004, 0x8000AAAAu);
        CHECK(static_cast<std::uint32_t>(m.call(sys_find_address, {0x80003000u, 0x80003008u, 0x80123456u})) ==
              0x80003000u);
    }
    SECTION("returns 0 when nothing matches") {
        m.set_word(0x4000, 1);
        m.set_word(0x4004, 2);
        m.set_word(0x4008, 3);
        CHECK(m.call(sys_find_address, {0x4000, 0x400C, 0xDEADBEEFu}) == 0);
    }
}

TEST_CASE("kernel system: SetSyscall mirrors the handler into the guest's syscall table",
          "[kernel][system]") {
    HostProgram p;
    TestMachine m(p.table());
    m.boot(k_main);
    // The probe words the SDK's patch code reads the table address from.
    CHECK(m.word(kabi::syscall_table_probe) == (kabi::syscall_table_guest >> 16));
    CHECK(m.word(kabi::syscall_table_probe + 8) == (kabi::syscall_table_guest & 0xFFFFu));
    constexpr std::uint32_t handler = 0x00383548u;
    constexpr std::uint32_t entry = kabi::syscall_table_guest + 0x82u * 4;
    CHECK(m.call(sys_set_syscall, {0x82u, handler}) == kabi::ok);
    CHECK(m.word(entry & 0x1FFFFFFFu) == handler);
    CHECK(m.machine->kernel().guest_handler(0x82) == handler);
    CHECK(static_cast<std::uint32_t>(m.call(sys_find_address, {0x80000000u, 0x80080000u, handler})) == entry);
    CHECK(static_cast<std::uint32_t>(m.call(sys_get_entry_address, {0x82u})) == handler);
    // A negative number reaches below the table (SDK kernel patches).
    CHECK(m.call(sys_set_syscall, {0xFFFFC402u, 0xDEADBEEFu}) == kabi::ok);
    CHECK(m.word(0x00002F88u) == 0xDEADBEEFu);
    // Handler 0 removes the guest handler.
    CHECK(m.call(sys_set_syscall, {0x82u, 0}) == kabi::ok);
    CHECK_FALSE(m.machine->kernel().guest_handler(0x82).has_value());
}

TEST_CASE("kernel system: syscall-table state belongs to one machine", "[kernel][system]") {
    HostProgram p;
    TestMachine first(p.table());
    TestMachine second(p.table());
    first.boot(k_main);
    second.boot(k_main);
    constexpr std::uint32_t entry = (kabi::syscall_table_guest + 0x5Au * 4) & 0x1FFFFFFFu;
    CHECK(first.call(sys_set_syscall, {0x5Au, 0x00383510u}) == kabi::ok);
    CHECK(first.machine->kernel().guest_handler(0x5A) == 0x00383510u);
    CHECK_FALSE(second.machine->kernel().guest_handler(0x5A).has_value());
    CHECK(second.call(sys_set_syscall, {0x5Au, 0x00383520u}) == kabi::ok);
    CHECK(first.word(entry) == 0x00383510u);
    CHECK(second.word(entry) == 0x00383520u);
}

TEST_CASE("kernel system: a SetSyscall handler runs as a nested guest call", "[kernel][system]") {
    HostProgram p;
    TestMachine m(p.table());
    prepare(m);
    SECTION("its $v0 comes back to the caller") {
        g.syscall = 0x91;
        g.args[0] = 7;
        g.args[1] = 5;
        const SliceResult r = run_override(m, p, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
            set_gpr_lo(*ctx, 2, gpr_u32(*ctx, 4) + gpr_u32(*ctx, 5));
            guest_return(ctx); // $ra 0: the nested call ends
        });
        CHECK(stop_of(r).reason == StopReason::requested);
        CHECK(g.results[0] == 12u);
    }
    SECTION("kseg arguments keep their sign extension") {
        g.syscall = 0x92;
        g.args[0] = 0x80000000u;
        g.args[1] = 0x80080000u;
        const SliceResult r = run_override(m, p, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
            SET_GPR_S32(ctx, 4, GPR_U32(ctx, 4) + 4u);
            SET_GPR_U64(ctx, 2, GPR_U64(ctx, 4) < GPR_U64(ctx, 5) ? 1u : 0u);
            if (GPR_U64(ctx, 2) == 0) {
                SET_GPR_U64(ctx, 4, 0);
            }
            SET_GPR_U64(ctx, 2, GPR_U64(ctx, 4));
            guest_return(ctx);
        });
        CHECK(stop_of(r).reason == StopReason::requested);
        CHECK(g.results[0] == 0x80000004u);
    }
    SECTION("the upper 64 bits of an argument register survive") {
        g.syscall = 0x93;
        p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
            ctx->r[4] = _mm_set_epi64x(static_cast<std::int64_t>(k_upper64),
                                       static_cast<std::int64_t>(static_cast<std::int32_t>(0x80000000u)));
            set_gpr_lo(*ctx, 3, g.syscall);
            rt->handleSyscall(rdram, ctx, 0);
            g.results[0] = gpr_u32(*ctx, 2);
            guest_request_stop(*g.machine);
        });
        p.set(k_handler, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
            const bool ok = gpr_hi(*ctx, 4) == k_upper64 &&
                            gpr_lo(*ctx, 4) == static_cast<std::uint64_t>(static_cast<std::int64_t>(
                                                   static_cast<std::int32_t>(0x80000000u)));
            set_gpr_lo(*ctx, 2, ok ? 1u : 0u);
            guest_return(ctx);
        });
        m.boot(k_main);
        CHECK(m.call(sys_set_syscall, {g.syscall, k_handler}) == kabi::ok);
        CHECK(stop_of(m.run()).reason == StopReason::requested);
        CHECK(g.results[0] == 1u);
    }
    SECTION("a jump to an address with no function stops the run (upstream completes it silently)") {
        g.syscall = 0x7F; // GetMemorySize: native, and not native_first (FindAddress and Copy are)
        const SliceResult r = run_override(m, p, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
            set_gpr_lo(*ctx, 2, 0xDEADBEEFu);
            ctx->pc = 0x12345678u;
        });
        CHECK(stop_of(r).reason == StopReason::missing_function);
        CHECK(r.stop->address == 0x12345678u);
    }
    SECTION("the same number inside the handler reaches the native service") {
        g.syscall = 0x7F; // GetMemorySize
        const SliceResult r = run_override(m, p, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
            set_gpr_lo(*ctx, 3, 0x7F);
            rt->handleSyscall(rdram, ctx, 0);
            guest_return(ctx);
        });
        CHECK(stop_of(r).reason == StopReason::requested);
        CHECK(g.results[0] == 0x02000000u);
    }
    SECTION("FindAddress stays native when the program installs its own (native_first)") {
        g.syscall = 0x83;
        g.args[0] = 0x3000;
        g.args[1] = 0x300C;
        g.args[2] = 0x11223344u;
        m.set_word(0x3000, 0xCAFEBABEu);
        m.set_word(0x3004, 0x11223344u);
        m.set_word(0x3008, 0x55667788u);
        const SliceResult r = run_override(m, p, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
            set_gpr_lo(*ctx, 2, 0xDEADBEEFu); // never runs
            guest_return(ctx);
        });
        CHECK(stop_of(r).reason == StopReason::requested);
        CHECK(g.results[0] == 0x3004u);
        CHECK(m.machine->kernel().guest_handler(0x83) == k_handler); // recorded all the same
    }
}

TEST_CASE("kernel system: a SetSyscall handler can block and resume without losing the caller's context",
          "[kernel][system]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(1);
        const auto driver = guest_create_thread(rdram, ctx, rt, k_param, k_driver, 0x1E000u, 0x1000u, 10);
        guest_syscall(rdram, ctx, rt, sys_start_thread, {static_cast<std::uint32_t>(driver), 0});
        set_gpr_lo(*ctx, 16, 0x5EEDu); // a callee-saved register the handler must not disturb
        g.results[0] = static_cast<std::uint32_t>(guest_syscall(rdram, ctx, rt, 0x94));
        g.trace.push_back(5);
        g.results[1] = gpr_u32(*ctx, 16);
        guest_request_stop(*g.machine);
    });
    p.set(k_handler, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(2);
        set_gpr_lo(*ctx, 16, 0xBADu);
        guest_syscall(rdram, ctx, rt, sys_sleep_thread); // the driver runs while we wait
        g.trace.push_back(4);
        set_gpr_lo(*ctx, 2, 0xB10C0EDu);
        guest_return(ctx);
    });
    p.set(k_driver, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        g.trace.push_back(3);
        guest_syscall(rdram, ctx, rt, sys_wakeup_thread, {1});
        guest_return(ctx);
    });
    TestMachine m(p.table());
    prepare(m);
    m.boot(k_main);
    CHECK(m.call(sys_set_syscall, {0x94u, k_handler}) == kabi::ok);
    CHECK(stop_of(m.run()).reason == StopReason::requested);
    CHECK(g.trace == std::vector<int>{1, 2, 3, 4, 5});
    CHECK(g.results[0] == 0xB10C0EDu);
    CHECK(g.results[1] == 0x5EEDu); // only $v0 comes back
}

TEST_CASE("kernel system: Copy and GetEntryAddress", "[kernel][system]") {
    HostProgram p;
    TestMachine m(p.table());
    m.boot(k_main);
    const std::uint32_t values[] = {0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u};
    for (std::uint32_t i = 0; i < 4; ++i) {
        m.set_word(0x6000 + 4 * i, values[i]);
    }
    CHECK(m.call(sys_copy, {0x5000, 0x6000, 16}) == kabi::ok);
    for (std::uint32_t i = 0; i < 4; ++i) {
        CHECK(m.word(0x5000 + 4 * i) == values[i]);
    }
    constexpr std::uint32_t entry = (kabi::syscall_table_guest + 0x5Au * 4) & 0x1FFFFFFFu;
    m.set_word(entry, 0x00383548u);
    CHECK(static_cast<std::uint32_t>(m.call(sys_get_entry_address, {0x5Au})) == 0x00383548u);
}

TEST_CASE("kernel system: the HLE kernel's reserved RAM is guarded", "[kernel][system]") {
    HostProgram p;
    SECTION("Copy into it stops") {
        TestMachine m(p.table());
        m.boot(k_main);
        try {
            m.call(sys_copy, {0x800E0100u, 0x6000, 16});
            FAIL("Copy returned");
        } catch (const GuestStop& stop) {
            CHECK(stop.info().reason == StopReason::kernel_ram_collision);
            CHECK(stop.info().message.find("Copy") != std::string::npos);
        }
    }
    SECTION("a SetSyscall handler inside it stops") {
        TestMachine m(p.table());
        m.boot(k_main);
        CHECK_THROWS_AS(m.call(sys_set_syscall, {0x95u, hle_handler_stack_base}), GuestStop);
    }
    SECTION("a guest store into it stops at the next vblank") {
        p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
            poke(rdram, hle_kernel_ram_base + 0x40, 0x12345678u);
            guest_syscall(rdram, ctx, rt, sys_sleep_thread);
        });
        TestMachine m(p.table());
        prepare(m);
        // Not a deadlock yet: an enabled handler could still wake the thread.
        m.boot(k_main);
        CHECK(m.call(0x10, {2, k_handler, 0, 0}) > 0);
        CHECK(m.call(0x14, {2}) == kabi::ok);
        const StopInfo stop = stop_of(m.run());
        CHECK(stop.reason == StopReason::kernel_ram_collision);
        CHECK(stop.address == hle_kernel_ram_base + 0x40);
    }
    SECTION("the sentinel is in place after boot") {
        TestMachine m(p.table());
        m.boot(k_main);
        CHECK(m.word(hle_kernel_ram_base) == hle_kernel_sentinel);
        CHECK(m.word(hle_kernel_ram_end - 4) == hle_kernel_sentinel);
    }
}

TEST_CASE("kernel system: the cache, TLB and DECI2 noops, and strict mode", "[kernel][system]") {
    HostProgram p;
    {
        TestMachine m(p.table());
        m.boot(k_main);
        CHECK(m.call(0x64, {0}) == kabi::ok);   // FlushCache
        CHECK(m.call(-0x68, {2}) == kabi::ok);  // iFlushCache
        CHECK(m.call(0x82) == kabi::ok);        // InitTLB
        CHECK(m.call(0x0D, {0, 0x1000}) == 0);  // SetVTLBRefillHandler
        CHECK(m.call(0x7C, {1, 0x2000}) == -1); // Deci2Call
        CHECK(m.call(-0x7C, {1, 0x2000}) == -1);
        const KernelServiceInfo* flush = m.machine->kernel().find("FlushCache");
        REQUIRE(flush != nullptr);
        CHECK((flush->flags & kernel_service_noop) != 0);
        CHECK_FALSE(flush->note.empty());
        CHECK(flush->owner == "kernel");
        CHECK(m.machine->kernel().call_count("Deci2Call") == 2);
    }
    MachineConfig strict;
    strict.strict_kernel = true;
    TestMachine m(p.table(), strict);
    m.boot(k_main);
    CHECK_THROWS_AS(m.call(0x64, {0}), GuestStop);
    CHECK(m.call(0x7F) == 0x02000000); // a real service is unaffected
}

TEST_CASE("kernel system: the exit paths stop the run", "[kernel][system]") {
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        const char path[] = "cdrom0:\\NEXT.ELF;1";
        std::memcpy(rdram + 0x7000, path, sizeof path);
        guest_syscall(rdram, ctx, rt, 0x06, {0x7000, 0, 0}); // LoadExecPS2
        guest_return(ctx);
    });
    TestMachine m(p.table());
    prepare(m);
    const StopInfo stop = stop_of(m.run_from(k_main));
    CHECK(stop.reason == StopReason::program_end);
    CHECK(stop.call_name == "LoadExecPS2");
    CHECK(stop.message.find("NEXT.ELF") != std::string::npos);
    TestMachine osd(p.table());
    osd.boot(k_main);
    CHECK_THROWS_AS(osd.call(0x7B), GuestStop); // ExecOSD
}

TEST_CASE("kernel system: every kernel call session A owns has a service", "[kernel][system]") {
    HostProgram p;
    TestMachine m(p.table());
    const char* const names[] = {
        // threads
        "CreateThread", "DeleteThread", "StartThread", "ExitThread", "ExitDeleteThread", "TerminateThread",
        "SuspendThread", "ResumeThread", "GetThreadId", "ReferThreadStatus", "iReferThreadStatus",
        "SleepThread", "WakeupThread", "iWakeupThread", "CancelWakeupThread", "iCancelWakeupThread",
        "ChangeThreadPriority", "iChangeThreadPriority", "RotateThreadReadyQueue", "iRotateThreadReadyQueue",
        "ReleaseWaitThread", "iReleaseWaitThread",
        // semaphores, event flags, alarms
        "CreateSema", "DeleteSema", "iDeleteSema", "SignalSema", "iSignalSema", "WaitSema", "PollSema",
        "iPollSema", "ReferSemaStatus", "iReferSemaStatus", "CreateEventFlag", "DeleteEventFlag",
        "SetEventFlag", "iSetEventFlag", "ClearEventFlag", "iClearEventFlag", "WaitEventFlag",
        "PollEventFlag", "iPollEventFlag", "ReferEventFlagStatus", "iReferEventFlagStatus", "SetAlarm",
        "iSetAlarm", "CancelAlarm", "iCancelAlarm",
        // interrupts
        "AddIntcHandler", "RemoveIntcHandler", "AddDmacHandler", "RemoveDmacHandler", "EnableIntc",
        "DisableIntc", "EnableDmac", "DisableDmac", "iEnableIntc", "iDisableIntc", "iEnableDmac",
        "iDisableDmac", "EnableIntcHandler", "DisableIntcHandler", "EnableDmacHandler", "DisableDmacHandler",
        // system
        "SetupThread", "SetupHeap", "EndOfHeap", "GetMemorySize", "GetOsdConfigParam", "SetOsdConfigParam",
        "GetOsdConfigParam2", "SetOsdConfigParam2", "SetSyscall", "Copy", "GetEntryAddress", "FindAddress",
        "FlushCache", "iFlushCache", "EnableCache", "DisableCache", "InitTLB", "SetVTLBRefillHandler",
        "Deci2Call", "LoadExecPS2", "ExecOSD"};
    for (const char* name : names) {
        INFO(name);
        const KernelServiceInfo* s = m.machine->kernel().find(name);
        REQUIRE(s != nullptr);
        CHECK(s->owner == "kernel");
    }
    // The SDK spellings resolve to the same services.
    for (const char* alias : {"AddIntcHandler2", "AddDmacHandler2", "ReleaseAlarm", "iReleaseAlarm"}) {
        INFO(alias);
        CHECK(m.machine->kernel().resolve_name(alias).kind == KernelResolution::Kind::native);
    }
}

TEST_CASE("kernel system: a call from the overlay window that returns into a replaced overlay stops",
          "[kernel][system]") {
    constexpr std::uint32_t window = 0x00400000u;
    HostProgram p;
    p.set(k_main, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        set_gpr_lo(*ctx, 31, k_main + 8);
        ctx->pc = window;
        if (!rt->dispatchGuestBranch(rdram, ctx, window, k_main, k_main + 8,
                                     PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
            return;
        }
        guest_request_stop(*g.machine);
    });
    p.set(k_main + 8, [](std::uint8_t*, R5900Context*, PS2Runtime*) { guest_request_stop(*g.machine); });
    // The "loader": swaps the resident overlay and returns normally.
    p.set(k_handler, [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
        g.overlays->set_resident(2);
        guest_return(ctx);
    });
    // Overlay 1 at the window's base calls the loader.
    static std::vector<dq8_guest_fn> slots1((0x2000) / 4, nullptr);
    static std::vector<dq8_guest_fn> slots2((0x2000) / 4, nullptr);
    slots1[0] = [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt) {
        set_gpr_lo(*ctx, 18, gpr_u32(*ctx, 31));
        set_gpr_lo(*ctx, 31, window + 0x10);
        ctx->pc = k_handler;
        if (!rt->dispatchGuestBranch(rdram, ctx, k_handler, window + 8, window + 0x10,
                                     PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
            return;
        }
        g.after_call = true; // overlay 1's code, running after overlay 2 replaced it
        set_gpr_lo(*ctx, 31, gpr_u32(*ctx, 18));
        guest_return(ctx);
    };
    slots2[4] = [](std::uint8_t*, R5900Context* ctx, PS2Runtime*) { guest_return(ctx); }; // window + 0x10
    TestMachine m(p.table());
    m.overlays = OverlayManager(OverlayWindowSpec{window, window + 0x2000});
    m.overlays.register_unit(1, "one", DispatchTable(window, window + 0x2000, slots1));
    m.overlays.register_unit(2, "two", DispatchTable(window, window + 0x2000, slots2));
    m.overlays.set_resident(1);
    prepare(m);
    g.overlays = &m.overlays;
    const StopInfo stop = stop_of(m.run_from(k_main));
    CHECK(stop.reason == StopReason::stale_return); // in every build since M4
    CHECK(to_string(stop.reason) == "stale_return");
    CHECK(stop.source == window + 8);
    CHECK_FALSE(g.after_call);
}
