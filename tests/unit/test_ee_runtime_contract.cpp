// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The host side of the dispatch contract, driven by a unit in the emitter's shape
// (support/contract_unit.cpp): calls nest on the host stack and return through dispatchGuestBranch, a jump
// unwinds to the run loop which re-enters through the jal+8 resume slot, pc 0 ends the program, kernel calls
// and by-name calls stop the guest with their registers, misses are explained, and special addresses reach
// the host.
#include "runtime/ee/context.h"
#include "runtime/ee/dispatcher.h"
#include "runtime/ee/ee_runtime.h"
#include "runtime/ee/game_module.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/ee/overlay_manager.h"
#include "runtime/kernel/kernel_hle.h"
#include "support/contract_unit.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace dq8::rt;

constexpr unsigned v0 = 2;
constexpr unsigned a0 = 4;
constexpr unsigned t2 = 10;
constexpr unsigned s0 = 16;

UnitSpec contract_spec() {
    UnitSpec spec;
    spec.name = contract::unit_name;
    spec.kind = DQ8_UNIT_MAIN;
    spec.source_sha1 = contract::source_sha1;
    spec.entry_point = contract::entry;
    spec.range_begin = contract::table_base;
    spec.range_end = contract::table_end;
    return spec;
}

struct Machine {
    GuestMemory memory;
    OverlayManager overlays;
    KernelHle kernel;
    Dispatcher dispatcher{overlays};
    EeRuntime ee{memory, dispatcher, kernel};
    std::vector<std::string> trace;

    Machine() {
        const dq8_game_module_desc* desc = contract_unit_descriptor();
        REQUIRE(validate_descriptor(desc, contract_spec()).empty());
        dispatcher.set_main(contract::unit_name, DispatchTable::from_descriptor(*desc));
        ee.set_trace_sink([this](std::string_view line) { trace.emplace_back(line); });
    }

    RunResult run_from(std::uint32_t pc, RunOptions options = {}) {
        reset_context(ee.context(), pc);
        options.trace_dispatch = true;
        return ee.run(options);
    }

    std::uint32_t reg(unsigned r) const { return gpr_u32(ee.context(), r); }
    bool traced(std::string_view needle) const {
        return std::any_of(trace.begin(), trace.end(),
                           [&](const std::string& l) { return l.find(needle) != std::string::npos; });
    }
};

} // namespace

TEST_CASE("contract: a call returns through dispatch and the syscall after it stops the guest",
          "[contract]") {
    Machine m;
    const RunResult r = m.run_from(contract::entry);
    REQUIRE(r.stop.reason == StopReason::syscall);
    CHECK(r.stop.number == 0x3Cu);
    CHECK(r.stop.call_name == "SetupThread");
    CHECK(r.stop.address == 0x00200014u); // the syscall instruction
    CHECK(r.stop.pc == 0x00200018u);      // generated code publishes the next pc first
    CHECK(r.stop.args[0] == 0x12345678u);
    CHECK(m.reg(a0) == 0x12345678u);
    CHECK(m.reg(v0) == 0x12345679u); // helper ran and returned into the caller's frame
    CHECK(r.dispatches == 2);        // the run loop's entry and the JAL
    CHECK(r.stop.last_branch == "JAL");
    CHECK(r.stop.last_target == 0x00200100u);
    CHECK(m.traced("JAL 0x00200008 -> 0x00200100 [contract]"));
    CHECK(r.stop.message.find("syscall 0x3C (SetupThread)") != std::string::npos);
}

TEST_CASE("contract: a jump unwinds to the run loop, which re-enters through the jal+8 resume slot",
          "[contract]") {
    Machine m;
    const RunResult r = m.run_from(contract::unwind_caller);
    REQUIRE(r.stop.reason == StopReason::syscall);
    CHECK(r.stop.number == 0x3Du); // reached only from the resume label
    CHECK(r.stop.call_name == "SetupHeap");
    CHECK(m.reg(s0) == 0x77u); // jump_target ran from the run loop
    CHECK(m.traced("JR 0x00200128 -> 0x00200140"));
    CHECK(m.traced("run-loop -> 0x00200140"));
    CHECK(m.traced("run-loop -> 0x00200048 [contract]"));
    // run loop, JAL, JR, run loop (jump target), run loop (resume entry)
    CHECK(r.dispatches == 5);
}

TEST_CASE("contract: pc 0 ends the program", "[contract]") {
    Machine m;
    const RunResult r = m.run_from(contract::ends);
    CHECK(r.stop.reason == StopReason::program_end);
    CHECK(m.reg(v0) == 7u);
    CHECK(m.ee.context().pc == 0u);
    CHECK(r.dispatches == 1);
}

TEST_CASE("contract: by-name calls stop with the call's name and arguments", "[contract]") {
    SECTION("a kernel call bound by name") {
        Machine m;
        const RunResult r = m.run_from(contract::calls_flush);
        REQUIRE(r.stop.reason == StopReason::named_call);
        CHECK(r.stop.call_kind == DQ8_CALL_SYSCALL);
        CHECK(r.stop.call_name == "FlushCache");
        CHECK(r.stop.has_number);
        CHECK(r.stop.number == 0x64u);
        CHECK(r.stop.pc == 0x0020009Cu); // the stub set pc to its return address first
        CHECK(r.stop.args[0] == 0u);
    }
    SECTION("a library stub") {
        Machine m;
        const RunResult r = m.run_from(contract::stub_memcpy);
        REQUIRE(r.stop.reason == StopReason::named_call);
        CHECK(r.stop.call_kind == DQ8_CALL_STUB);
        CHECK(r.stop.call_name == "memcpy");
        CHECK_FALSE(r.stop.has_number);
        CHECK(r.stop.message.find("stub call memcpy") != std::string::npos);
    }
    SECTION("a stub the kernel HLE implements runs and the guest continues") {
        Machine m;
        m.kernel.register_handler("memcpy",
                                  [](std::uint8_t*, R5900Context* ctx) { set_gpr_lo(*ctx, 2, 99); });
        const RunResult r = m.run_from(contract::stub_memcpy);
        CHECK(r.stop.reason == StopReason::program_end); // ra was 0
        CHECK(m.reg(v0) == 99u);
    }
}

TEST_CASE("contract: an implemented syscall continues, unless stop-at-first-syscall is set", "[contract]") {
    Machine m;
    int calls = 0;
    m.kernel.register_handler("SetupThread", [&calls](std::uint8_t*, R5900Context*) { ++calls; });
    SECTION("continues; the dispatch budget ends the loop the program falls into") {
        RunOptions o;
        o.max_dispatches = 10;
        const RunResult r = m.run_from(contract::entry, o);
        CHECK(r.stop.reason == StopReason::max_dispatches);
        CHECK(r.dispatches == 10);
        CHECK(calls > 1);
    }
    SECTION("stop-at-first-syscall stops before the handler") {
        RunOptions o;
        o.stop_at_first_syscall = true;
        const RunResult r = m.run_from(contract::entry, o);
        CHECK(r.stop.reason == StopReason::syscall);
        CHECK(calls == 0);
    }
}

TEST_CASE("contract: a dispatch to an address with no function stops with a message", "[contract]") {
    SECTION("a call to an empty slot") {
        Machine m;
        const RunResult r = m.run_from(contract::calls_missing);
        REQUIRE(r.stop.reason == StopReason::missing_function);
        CHECK(r.stop.address == contract::missing_target);
        CHECK(r.stop.source == 0x002000E0u);
        const std::string& msg = r.stop.message;
        CHECK(msg.find("no recompiled function at 0x002001F0") != std::string::npos);
        CHECK(msg.find("contract unit's table") != std::string::npos);
        CHECK(msg.find("JAL from 0x002000E0") != std::string::npos);
    }
    SECTION("the run loop at an address outside every table") {
        Machine m;
        const RunResult r = m.run_from(0x00300000u);
        REQUIRE(r.stop.reason == StopReason::missing_function);
        CHECK(r.stop.message.find("outside every dispatch table") != std::string::npos);
        CHECK(r.stop.message.find("run loop") != std::string::npos);
    }
}

TEST_CASE("contract: a store to a scratchpad literal reaches the scratchpad", "[contract]") {
    Machine m;
    const RunResult r = m.run_from(contract::scratchpad_store);
    CHECK(r.stop.reason == StopReason::program_end);
    std::uint32_t sp_word = 0;
    std::memcpy(&sp_word, m.memory.scratchpad() + 0x10, sizeof sp_word);
    CHECK(sp_word == 0x5A5Au);
    std::uint32_t ram_word = 0;
    std::memcpy(&ram_word, m.memory.rdram() + 0x10, sizeof ram_word);
    CHECK(ram_word == 0u); // not the RAM fast path's masked address
    CHECK(m.reg(t2) == 0x5A5Au);
    CHECK(m.memory.mmio_log().empty());
}

TEST_CASE("contract: an exception from generated code is a guest error", "[contract]") {
    Machine m;
    const RunResult r = m.run_from(contract::unhandled);
    CHECK(r.stop.reason == StopReason::guest_error);
    CHECK(r.stop.message.find("Unhandled opcode") != std::string::npos);
    CHECK(r.stop.pc == contract::unhandled);
}

TEST_CASE("contract: the call-depth guard stops runaway recursion", "[contract]") {
    Machine m;
    RunOptions o;
    o.max_call_depth = 64;
    const RunResult r = m.run_from(contract::recurse, o);
    CHECK(r.stop.reason == StopReason::call_depth);
    CHECK(r.dispatches == 66); // the run loop's entry, 64 nested calls, and the one refused
    // A second run on the same runtime starts from depth 0 again.
    const RunResult again = m.run_from(contract::ends);
    CHECK(again.stop.reason == StopReason::program_end);
}

TEST_CASE("contract: the api table forwards every member", "[contract]") {
    const dq8_runtime_api& api = EeRuntime::api();
    CHECK(api.struct_size == sizeof(dq8_runtime_api));
    CHECK(api.abi_version == DQ8_GAME_MODULE_ABI_VERSION);
    Machine m;
    PS2Runtime* rt = m.ee.adapter();
    CHECK(rt->api == &api);
    R5900Context& ctx = m.ee.context();
    std::uint8_t* rdram = m.memory.rdram();
    CHECK_FALSE(rt->eeCheckpointDue());
    rt->Store64(rdram, &ctx, 0x70000020u, 0x1122334455667788ull);
    CHECK(rt->Load64(rdram, &ctx, 0x70000020u) == 0x1122334455667788ull);
    CHECK(rt->Load16(rdram, &ctx, 0x70000022u) == 0x5566u);
    rt->Store8(rdram, &ctx, 0x80000040u, 0xAB); // kseg0 mirror of RAM 0x40
    CHECK(rdram[0x40] == 0xAB);
    ctx.llbit = 1;
    rt->clearLLBit(&ctx);
    CHECK(ctx.llbit == 0);
    CHECK_THROWS_AS(rt->handleTLBWI(rdram, &ctx), GuestStop);
    CHECK_THROWS_AS(rt->handleBreak(rdram, &ctx), GuestStop);
    CHECK_THROWS_AS(rt->executeVU0Microprogram(rdram, &ctx, 0), GuestStop);
    try {
        rt->SignalException(&ctx, EXCEPTION_INTEGER_OVERFLOW);
        FAIL("SignalException returned");
    } catch (const GuestStop& stop) {
        CHECK(stop.info().reason == StopReason::guest_exception);
        CHECK(stop.info().number == EXCEPTION_INTEGER_OVERFLOW);
        CHECK(stop.info().message.find("integer overflow") != std::string::npos);
    }
}
