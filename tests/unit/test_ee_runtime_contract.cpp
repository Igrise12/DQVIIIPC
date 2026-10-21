// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The host side of the dispatch contract, driven by a unit in the emitter's shape (support/contract_unit.cpp)
// on a real Machine: calls nest on a fiber's stack and return through dispatchGuestBranch, a jump unwinds to
// the run loop which re-enters through the jal+8 resume slot, pc 0 ends the program, kernel calls go through
// KernelServices, misses are explained, and special addresses reach the host. Since M3 also: two threads and
// a semaphore, a vblank handler, an interrupt taken at a back-edge, the unwinding flag's regression cases,
// and run-to-run determinism.
#include "runtime/ee/context.h"
#include "runtime/ee/game_module.h"
#include "runtime/kernel/kernel_services.h"
#include "runtime/machine/events.h"
#include "support/contract_unit.h"
#include "support/machine_harness.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace dq8::rt;
using dq8::test::TestMachine;
using dq8::test::VblankIrqDevice;

constexpr unsigned v0 = 2;
constexpr unsigned a0 = 4;
constexpr unsigned t2 = 10;
constexpr unsigned s0 = 16;
constexpr unsigned s1 = 17;

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

DispatchTable contract_table() {
    const dq8_game_module_desc* desc = contract_unit_descriptor();
    REQUIRE(validate_descriptor(desc, contract_spec()).empty());
    return DispatchTable::from_descriptor(*desc);
}

struct Contract : TestMachine {
    std::vector<std::string> trace;

    explicit Contract(MachineConfig config = {})
        : TestMachine(contract_table(), config, contract::unit_name) {
        machine->ee().set_trace_sink([this](std::string_view line) { trace.emplace_back(line); });
        contract::counters() = contract::Counters{};
    }

    // M2's run: dispatch tracing on, every dispatch limit from the options.
    SliceResult run_m2(std::uint32_t pc, std::uint64_t max_dispatches = 0) {
        RunLimits limits;
        limits.max_dispatches = max_dispatches;
        return run_from(pc, limits);
    }

    bool traced(std::string_view needle) const {
        return std::any_of(trace.begin(), trace.end(),
                           [&](const std::string& l) { return l.find(needle) != std::string::npos; });
    }
};

MachineConfig traced_config() {
    MachineConfig c;
    c.trace_dispatch = true;
    return c;
}

MachineConfig stop_at_syscall() {
    MachineConfig c = traced_config();
    c.stop_at_first_syscall = true;
    return c;
}

bool same_events(const std::vector<MachineEvent>& a, const std::vector<MachineEvent>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](const MachineEvent& x, const MachineEvent& y) {
                          return x.type == y.type && x.fields == y.fields;
                      });
}

StopInfo stop_of(const SliceResult& r) {
    REQUIRE(r.stopped);
    REQUIRE(r.stop.has_value());
    return *r.stop;
}

} // namespace

// ---- M2 cases, on the Machine.

TEST_CASE("contract: a call returns through dispatch and the syscall after it stops at the first syscall",
          "[contract]") {
    Contract m(stop_at_syscall());
    const SliceResult r = m.run_m2(contract::entry);
    const StopInfo stop = stop_of(r);
    REQUIRE(stop.reason == StopReason::syscall);
    CHECK(stop.number == 0x3Cu);
    CHECK(stop.call_name == "SetupThread");
    CHECK(stop.address == 0x00200014u); // the syscall instruction
    CHECK(stop.pc == 0x00200018u);      // generated code publishes the next pc first
    CHECK(stop.args[0] == 0x12345678u);
    CHECK(stop.thread_id == 1);
    CHECK(m.reg(a0) == 0x12345678u);
    CHECK(m.reg(v0) == 0x12345679u); // helper ran and returned into the caller's frame
    CHECK(r.dispatches == 2);        // the run loop's entry and the JAL
    CHECK(stop.last_branch == "JAL");
    CHECK(stop.last_target == 0x00200100u);
    CHECK(m.traced("JAL 0x00200008 -> 0x00200100 [contract]"));
    CHECK(stop.message.find("syscall 0x3C (SetupThread)") != std::string::npos);
}

TEST_CASE("contract: a jump unwinds to the run loop, which re-enters through the jal+8 resume slot",
          "[contract]") {
    Contract m(stop_at_syscall());
    const SliceResult r = m.run_m2(contract::unwind_caller);
    const StopInfo stop = stop_of(r);
    REQUIRE(stop.reason == StopReason::syscall);
    CHECK(stop.number == 0x3Du); // reached only from the resume label
    CHECK(stop.call_name == "SetupHeap");
    CHECK(m.reg(s0) == 0x77u); // jump_target ran from the run loop
    CHECK(m.traced("JR 0x00200128 -> 0x00200140"));
    CHECK(m.traced("run-loop -> 0x00200140"));
    CHECK(m.traced("run-loop -> 0x00200048 [contract]"));
    // run loop, JAL, JR, run loop (jump target), run loop (resume entry)
    CHECK(r.dispatches == 5);
}

TEST_CASE("contract: pc 0 ends the program", "[contract]") {
    Contract m(traced_config());
    const SliceResult r = m.run_m2(contract::ends);
    CHECK(stop_of(r).reason == StopReason::program_end);
    CHECK(m.reg(v0) == 7u);
    CHECK(m.ctx().pc == 0u);
    CHECK(r.dispatches == 1);
}

TEST_CASE("contract: by-name calls reach KernelServices or stop with the call's name and arguments",
          "[contract]") {
    SECTION("a kernel call bound by name stops at the first syscall") {
        Contract m(stop_at_syscall());
        const StopInfo stop = stop_of(m.run_m2(contract::calls_flush));
        REQUIRE(stop.reason == StopReason::named_call);
        CHECK(stop.call_kind == DQ8_CALL_SYSCALL);
        CHECK(stop.call_name == "FlushCache");
        CHECK(stop.has_number);
        CHECK(stop.number == 0x64u);
        CHECK(stop.pc == 0x0020009Cu); // the stub set pc to its return address first
        CHECK(stop.args[0] == 0u);
    }
    SECTION("without it, the FlushCache noop returns 0 and the guest continues") {
        Contract m(traced_config());
        CHECK(m.ctx().pc == 0u); // before boot: a power-on context
        // calls_flush returns through the $ra its own jal overwrote, so it loops on its return label.
        const SliceResult r = m.run_m2(contract::calls_flush, 8);
        CHECK(stop_of(r).reason == StopReason::max_dispatches);
        CHECK(m.reg(v0) == 0u);
        CHECK(m.machine->kernel().call_count("FlushCache") == 1);
    }
    SECTION("strict mode stops at the noop") {
        MachineConfig c = traced_config();
        c.strict_kernel = true;
        Contract m(c);
        const StopInfo stop = stop_of(m.run_m2(contract::calls_flush));
        CHECK(stop.reason == StopReason::named_call);
        CHECK(stop.message.find("noop in strict mode") != std::string::npos);
    }
    SECTION("a library stub without a service") {
        Contract m(traced_config());
        const StopInfo stop = stop_of(m.run_m2(contract::stub_memcpy));
        REQUIRE(stop.reason == StopReason::named_call);
        CHECK(stop.call_kind == DQ8_CALL_STUB);
        CHECK(stop.call_name == "memcpy");
        CHECK_FALSE(stop.has_number);
        CHECK(stop.message.find("stub call memcpy") != std::string::npos);
    }
    SECTION("a stub a service implements runs and the guest continues") {
        Contract m(traced_config());
        m.machine->kernel().add(
            KernelServiceInfo{"memcpy", [](KernelCallContext& c) { c.ret(99); }, 0, {}, "test"});
        const SliceResult r = m.run_m2(contract::stub_memcpy);
        CHECK(stop_of(r).reason == StopReason::program_end); // ra was 0
        CHECK(m.reg(v0) == 99u);
    }
}

TEST_CASE("contract: an implemented syscall continues, unless stop-at-first-syscall is set", "[contract]") {
    SECTION("continues; the dispatch budget ends the loop the program falls into") {
        Contract m(traced_config());
        const SliceResult r = m.run_m2(contract::entry, 10);
        CHECK(stop_of(r).reason == StopReason::max_dispatches);
        CHECK(r.dispatches == 10);
        CHECK(m.machine->kernel().call_count("SetupThread") > 1);
    }
    SECTION("stop-at-first-syscall stops before the service") {
        Contract m(stop_at_syscall());
        const SliceResult r = m.run_m2(contract::entry);
        CHECK(stop_of(r).reason == StopReason::syscall);
        CHECK(m.machine->kernel().call_count("SetupThread") == 0);
    }
}

TEST_CASE("contract: a syscall no service implements stops with its number", "[contract]") {
    dq8::test::HostProgram program;
    program.set(
        dq8::test::HostProgram::base, [](std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
            ctx->pc = dq8::test::HostProgram::base + 8;
            dq8::test::guest_syscall(rdram, ctx, runtime, 0x01, {0x7Fu}); // ResetEE: nobody registers it
            dq8::test::guest_return(ctx);
        });
    TestMachine m(program.table());
    const StopInfo stop = stop_of(m.run_from(dq8::test::HostProgram::base));
    CHECK(stop.reason == StopReason::syscall);
    CHECK(stop.call_name == "ResetEE");
    CHECK(stop.args[0] == 0x7Fu);
    CHECK(stop.message.find("has no handler for it") != std::string::npos);
}

TEST_CASE("contract: a dispatch to an address with no function stops with a message", "[contract]") {
    SECTION("a call to an empty slot") {
        Contract m(traced_config());
        const StopInfo stop = stop_of(m.run_m2(contract::calls_missing));
        REQUIRE(stop.reason == StopReason::missing_function);
        CHECK(stop.address == contract::missing_target);
        CHECK(stop.source == 0x002000E0u);
        CHECK(stop.message.find("no recompiled function at 0x002001F0") != std::string::npos);
        CHECK(stop.message.find("contract unit's table") != std::string::npos);
        CHECK(stop.message.find("JAL from 0x002000E0") != std::string::npos);
    }
    SECTION("the run loop at an address outside every table") {
        Contract m(traced_config());
        const StopInfo stop = stop_of(m.run_m2(0x00300000u));
        REQUIRE(stop.reason == StopReason::missing_function);
        CHECK(stop.message.find("outside every dispatch table") != std::string::npos);
        CHECK(stop.message.find("run loop") != std::string::npos);
    }
}

TEST_CASE("contract: a store to a scratchpad literal reaches the scratchpad", "[contract]") {
    Contract m(traced_config());
    const SliceResult r = m.run_m2(contract::scratchpad_store);
    CHECK(stop_of(r).reason == StopReason::program_end);
    std::uint32_t sp_word = 0;
    std::memcpy(&sp_word, m.memory.scratchpad() + 0x10, sizeof sp_word);
    CHECK(sp_word == 0x5A5Au);
    std::uint32_t ram_word = 0;
    std::memcpy(&ram_word, m.memory.rdram() + 0x10, sizeof ram_word);
    CHECK(ram_word == 0u); // not the RAM fast path's masked address
    CHECK(m.reg(t2) == 0x5A5Au);
    CHECK(m.memory.mmio_log().empty());
}

TEST_CASE("contract: an exception from generated code is a guest error, caught on its fiber", "[contract]") {
    Contract m(traced_config());
    const StopInfo stop = stop_of(m.run_m2(contract::unhandled));
    CHECK(stop.reason == StopReason::guest_error);
    CHECK(stop.message.find("Unhandled opcode") != std::string::npos);
    CHECK(stop.pc == contract::unhandled);
    CHECK(stop.thread_id == 1);
}

TEST_CASE("contract: the call-depth guard stops runaway recursion", "[contract]") {
    MachineConfig c = traced_config();
    c.max_call_depth = 64;
    Contract m(c);
    const SliceResult r = m.run_m2(contract::recurse);
    CHECK(stop_of(r).reason == StopReason::call_depth);
    CHECK(r.dispatches == 66); // the run loop's entry, 64 nested calls, and the one refused
    // A second boot of the same machine starts over: new threads, depth 0, the dispatch count kept.
    const SliceResult again = m.run_from(contract::ends);
    CHECK(stop_of(again).reason == StopReason::program_end);
}

TEST_CASE("contract: the api table forwards every member", "[contract]") {
    const dq8_runtime_api& api = EeRuntime::api();
    CHECK(api.struct_size == sizeof(dq8_runtime_api));
    CHECK(api.abi_version == DQ8_GAME_MODULE_ABI_VERSION);
    Contract m;
    m.boot(contract::ends);
    PS2Runtime* rt = m.machine->ee().adapter();
    CHECK(rt->api == &api);
    R5900Context& ctx = m.ctx();
    std::uint8_t* rdram = m.memory.rdram();
    CHECK_FALSE(rt->eeCheckpointDue()); // off any fiber: charges the clock, switches nothing
    rt->Store64(rdram, &ctx, 0x70000020u, 0x1122334455667788ull);
    CHECK(rt->Load64(rdram, &ctx, 0x70000020u) == 0x1122334455667788ull);
    CHECK(rt->Load16(rdram, &ctx, 0x70000022u) == 0x5566u);
    rt->Store8(rdram, &ctx, 0x80000040u, 0xAB); // kseg0 mirror of RAM 0x40
    CHECK(rdram[0x40] == 0xAB);
    ctx.llbit = 1;
    rt->clearLLBit(&ctx);
    CHECK(ctx.llbit == 0);
    // Off any fiber a stop throws (the scheduler loop would catch it).
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

// ---- M3: the execution core.

TEST_CASE("contract: two threads with a semaphore", "[contract][m3]") {
    Contract m;
    const SliceResult r = m.run_from(contract::two_threads);
    CHECK(stop_of(r).reason == StopReason::program_end);
    const std::uint32_t sema = m.word(contract::data + 0x80u);
    CHECK(static_cast<std::int32_t>(sema) > 0);
    CHECK(m.reg(s0) == sema);   // WaitSema returned the semaphore id
    CHECK(m.reg(s1) == 0xB0Bu); // the worker ran before the waiter resumed
    const auto started = std::count_if(m.events.begin(), m.events.end(), [](const MachineEvent& e) {
        return e.type == "thread" && !e.fields.empty() &&
               e.fields[0].second == MachineEvent::Value(std::string("start"));
    });
    CHECK(started == 1);
    // The waiter (priority 0) preempted the worker at the end of its SignalSema; the worker never exited.
    const GuestThread* worker = m.kernel().thread(2);
    REQUIRE(worker != nullptr);
    CHECK(worker->status == ThreadStatus::ready);
}

TEST_CASE("contract: a vblank handler that iSignalSemas wakes the waiting thread", "[contract][m3]") {
    Contract m;
    VblankIrqDevice vblank_irq; // INTC 2 from field 1 on
    m.machine->attach(vblank_irq);
    const SliceResult r = m.run_from(contract::vblank_sema);
    CHECK(stop_of(r).reason == StopReason::program_end);
    const std::uint32_t sema = m.word(contract::data + 0x80u);
    CHECK(m.reg(s0) == sema);
    CHECK(m.reg(s1) == 1u); // the handler ran once before the waiter resumed
    CHECK(r.cycles >= m.machine->video().field_start(1));
    CHECK(r.vblank == 2); // fields 0 and 1 started
}

TEST_CASE("contract: an interrupt is taken at a back-edge checkpoint with no return cascade",
          "[contract][m3]") {
    Contract m;
    VblankIrqDevice vblank_irq;
    m.machine->attach(vblank_irq);
    const SliceResult r = m.run_from(contract::backedge_irq);
    CHECK(stop_of(r).reason == StopReason::program_end);
    CHECK(m.reg(s1) == 1u);                              // the handler set the flag the loop polled
    CHECK(contract::counters().spin_loop_entries == 1u); // never left and re-entered through the run loop
    // The run loop, the JAL, and the run-loop entry of the handler: no resume dispatches.
    CHECK(r.dispatches == 3);
}

TEST_CASE("contract: regression - a self-entry back-edge with a forced safe point", "[contract][m3]") {
    Contract m;
    m.boot(contract::self_loop_caller);
    // Mid-loop, an event raises INTC 9 (a timer) whose handler the program registered: the back-edge
    // checkpoint switches to the handler fiber inside the call and returns 0.
    m.machine->events().schedule(5000, "test-timer",
                                 [&m](Cycles) { m.machine->interrupts().raise_intc(IntcLine::timer0); });
    const SliceResult r = m.run();
    CHECK(stop_of(r).reason == StopReason::program_end);
    CHECK(m.reg(s0) == contract::self_loop_iterations); // every iteration ran
    CHECK(m.reg(s1) == 0x5E1Fu);                        // and the caller continued after the call
    CHECK(m.word(contract::data + 0x94u) == 1u);        // the handler ran mid-loop
    CHECK(contract::counters().self_loop_entries == 1u);
}

TEST_CASE("contract: regression - a jr to the callee's own entry re-enters it through the run loop",
          "[contract][m3]") {
    Contract m;
    const SliceResult r = m.run_from(contract::jr_self_caller);
    CHECK(stop_of(r).reason == StopReason::program_end);
    CHECK(m.reg(s0) == 3u); // M2's "pc == target means returned" rule would stop at 1
    CHECK(m.reg(s1) == 0x15E1u);
    CHECK(contract::counters().jr_self_entries == 3u);
}

TEST_CASE("contract: the same program twice gives the same event trace", "[contract][m3]") {
    const auto run_once = [](std::uint32_t entry) {
        MachineConfig c;
        c.trace_mask = trace_bit(TraceCategory::kernel);
        Contract m(c);
        VblankIrqDevice vblank_irq;
        m.machine->attach(vblank_irq);
        const SliceResult r = m.run_from(entry);
        return std::make_tuple(m.events, r.cycles, r.dispatches, r.stop->message);
    };
    for (const std::uint32_t entry : {contract::two_threads, contract::vblank_sema, contract::backedge_irq}) {
        const auto first = run_once(entry);
        const auto second = run_once(entry);
        CHECK(std::get<0>(first).size() >= 2); // kernel-call traces at least
        CHECK(same_events(std::get<0>(first), std::get<0>(second)));
        CHECK(std::get<1>(first) == std::get<1>(second));
        CHECK(std::get<2>(first) == std::get<2>(second));
        CHECK(std::get<3>(first) == std::get<3>(second));
    }
}

TEST_CASE("contract: a slice ends at a vblank limit and the next run resumes", "[contract][m3]") {
    Contract m;
    VblankIrqDevice vblank_irq;
    m.machine->attach(vblank_irq);
    m.boot(contract::vblank_sema);
    RunLimits limits;
    limits.until_vblank = 1; // field 0 starts at cycle 0
    const SliceResult first = m.run(limits);
    CHECK_FALSE(first.stopped);
    CHECK(first.vblank == 1);
    const SliceResult second = m.run({});
    CHECK(stop_of(second).reason == StopReason::program_end);
    CHECK(m.reg(s1) == 1u);
}
