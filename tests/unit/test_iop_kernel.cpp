// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_stop.h"
#include "runtime/iop/iop_exec.h"
#include "runtime/iop/kernel/hle.h"
#include "runtime/iop/kernel/iop_printf.h"
#include "support/irx_builder.h"
#include "support/sif_client.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace rt = dq8::rt;
namespace m = dq8::test::mips;
using Catch::Matchers::ContainsSubstring;
using dq8::test::IopRig;
using dq8::test::IrxBuilder;
using dq8::test::SifClient;
using Label = IrxBuilder::Label;

namespace {

// thbase's CreateThread parameter block for `entry`.
Label thread_param(IrxBuilder& b, Label entry, std::uint32_t priority, std::uint32_t stack = 0x800) {
    const Label p = b.words({0, 0, 0, stack, priority});
    b.pointer(p, 8, entry);
    return p;
}

// CreateThread + StartThread(id, arg); needs thbase 4 and 6 imported.
void spawn(IrxBuilder& b, Label param, std::uint32_t arg) {
    b.la(m::a0, param);
    b.jal(b.stub("thbase", 4));
    b.emit(m::move(m::a0, m::v0));
    b.li(m::a1, arg);
    b.jal(b.stub("thbase", 6));
}

std::string path_of(const std::string& name) {
    return "cdrom0:\\MODULES\\" + name + ".IRX;1";
}

std::uint32_t base_of(IopRig& rig, std::string_view name) {
    for (const auto& mod : rig.iop->interpreter().modules()) {
        if (mod.name == name) {
            return mod.base;
        }
    }
    throw std::logic_error("module " + std::string(name) + " is not loaded");
}

std::vector<std::uint32_t> read_log(IopRig& rig, std::uint32_t at) {
    std::vector<std::uint32_t> out;
    const std::uint32_t n = dq8::test::iop_ram32(*rig.iop, at);
    for (std::uint32_t i = 0; i < n && i < 64; ++i) {
        out.push_back(dq8::test::iop_ram32(*rig.iop, at + 4 + 4 * i));
    }
    return out;
}

const rt::MachineEvent* last_event(const dq8::test::FakeMachine& mach, std::string_view type) {
    for (auto it = mach.emitted.rbegin(); it != mach.emitted.rend(); ++it) {
        if (it->type == type) {
            return &*it;
        }
    }
    return nullptr;
}

std::string field(const rt::MachineEvent& e, std::string_view key) {
    for (const auto& [k, v] : e.fields) {
        if (k == key) {
            return std::visit(
                [](const auto& x) -> std::string {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, std::string>) {
                        return x;
                    } else if constexpr (std::is_same_v<T, bool>) {
                        return x ? "true" : "false";
                    } else {
                        return std::to_string(x);
                    }
                },
                v);
        }
    }
    return "<missing>";
}

std::uint32_t s32(std::int32_t v) {
    return static_cast<std::uint32_t>(v);
}

// ---- modules
// ------------------------------------------------------------------------------------------------

// Start: records argc and strlen(argv[1]), prints, sleeps 1 ms, records the system time, returns 0.
struct Basic {
    IrxBuilder b{"BASIC"};
    Label out;
    Basic() {
        const Label msg = b.string("hello %d from %s, %04x|%-6.3s|\n");
        const Label who = b.string("BASIC");
        out = b.zeros(32);
        b.imports("stdio", {4});
        b.imports("thbase", {33, 34});
        b.imports("sysclib", {27});
        const Label start = b.here();
        b.enter(32);
        b.store(out, 0, m::a0);
        b.emit(m::move(m::s0, m::a1));
        b.emit(m::lw(m::a0, 4, m::s0));
        b.emit(m::nop());
        b.jal(b.stub("sysclib", 27));
        b.store(out, 4, m::v0);
        b.la(m::a0, msg);
        b.li(m::a1, 42);
        b.la(m::a2, who);
        b.li(m::a3, 0xBE);
        b.la(m::t0, who);
        b.emit(m::sw(m::t0, 16, m::sp));
        b.jal(b.stub("stdio", 4));
        b.store(out, 8, m::v0);
        b.call(b.stub("thbase", 33), {1000});
        b.la(m::a0, out);
        b.emit(m::addiu(m::a0, m::a0, 16));
        b.jal(b.stub("thbase", 34));
        b.li(m::v0, 0);
        b.leave(32);
        b.entry(start);
    }
};

// Two threads and a semaphore: the waiter (priority 20) blocks, the signaller (30) wakes it and is preempted.
struct Threads {
    IrxBuilder b{"THREADS"};
    Label log;
    Threads() {
        log = b.zeros(64);
        const Label sema_param = b.words({0, 0, 0, 5});
        const Label sema = b.zeros(4);
        b.imports("thbase", {4, 6});
        b.imports("thsemap", {4, 6, 8});
        const Label waiter = b.label();
        const Label signaller = b.label();
        const Label p_waiter = thread_param(b, waiter, 20);
        const Label p_signaller = thread_param(b, signaller, 30);

        const Label start = b.here();
        b.enter(32);
        b.la(m::a0, sema_param);
        b.jal(b.stub("thsemap", 4));
        b.store(sema, 0, m::v0);
        spawn(b, p_waiter, 0x11);
        spawn(b, p_signaller, 0x22);
        b.append_value(log, 0x10);
        b.li(m::v0, 0);
        b.leave(32);

        b.bind(waiter);
        b.enter(32);
        b.emit(m::move(m::s0, m::a0));
        b.append(log, m::s0);
        b.load(m::a0, sema);
        b.jal(b.stub("thsemap", 8));
        b.append_value(log, 3);
        b.leave(32);

        b.bind(signaller);
        b.enter(32);
        b.append_value(log, 2);
        b.load(m::a0, sema);
        b.jal(b.stub("thsemap", 6));
        b.append_value(log, 4);
        b.leave(32);
        b.entry(start);
    }
};

// A hardware timer at 1 ms whose handler counts to five, then wakes the thread and stops the timer.
struct Timer {
    IrxBuilder b{"TIMER"};
    Label log;
    Label count;
    Label stamps;
    Label seen;
    Timer() {
        log = b.zeros(64);
        count = b.zeros(4);
        stamps = b.zeros(16);
        seen = b.zeros(4);
        const Label tid = b.zeros(4);
        b.imports("thbase", {4, 6, 20, 24, 26, 34});
        b.imports("timrman", {4, 6, 20, 22, 23, 24});
        const Label body = b.label();
        const Label handler = b.label();
        const Label p_body = thread_param(b, body, 20);

        const Label start = b.here();
        b.enter(32);
        spawn(b, p_body, 0);
        b.li(m::v0, 0);
        b.leave(32);

        b.bind(body);
        b.enter(32);
        b.jal(b.stub("thbase", 20));
        b.store(tid, 0, m::v0);
        b.call(b.stub("timrman", 4), {1, 32, 1});
        b.emit(m::move(m::s0, m::v0));
        b.emit(m::move(m::a0, m::s0));
        b.li(m::a1, 36864);
        b.la(m::a2, handler);
        b.li(m::a3, 0x77);
        b.jal(b.stub("timrman", 20));
        b.append(log, m::v0);
        b.emit(m::move(m::a0, m::s0));
        b.li(m::a1, 1);
        b.li(m::a2, 0);
        b.li(m::a3, 1);
        b.jal(b.stub("timrman", 22));
        b.append(log, m::v0);
        b.la(m::a0, stamps);
        b.jal(b.stub("thbase", 34));
        b.emit(m::move(m::a0, m::s0));
        b.jal(b.stub("timrman", 23));
        b.append(log, m::v0);
        b.jal(b.stub("thbase", 24));
        b.la(m::a0, stamps);
        b.emit(m::addiu(m::a0, m::a0, 8));
        b.jal(b.stub("thbase", 34));
        b.emit(m::move(m::a0, m::s0));
        b.jal(b.stub("timrman", 24));
        b.append(log, m::v0);
        b.emit(m::move(m::a0, m::s0));
        b.jal(b.stub("timrman", 6));
        b.append(log, m::v0);
        b.leave(32);

        b.bind(handler);
        b.enter(32);
        b.store(seen, 0, m::a0);
        b.load(m::t0, count);
        b.emit(m::addiu(m::t0, m::t0, 1));
        b.store(count, 0, m::t0);
        b.emit(m::slti(m::t1, m::t0, 5));
        const Label again = b.label();
        b.bne(m::t1, m::zero, again);
        b.load(m::a0, tid);
        b.jal(b.stub("thbase", 26));
        b.li(m::v0, 0);
        b.leave(32);
        b.bind(again);
        b.li(m::v0, 36864);
        b.leave(32);
        b.entry(start);
    }
};

IopRig make_rig(std::initializer_list<std::pair<std::string, const IrxBuilder*>> modules,
                const std::function<void(rt::IopConfig&)>& configure = {}) {
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files;
    std::vector<rt::IopModuleSpec> specs = dq8::test::test_modules();
    for (const auto& [name, builder] : modules) {
        files.emplace_back(name, builder->build());
        specs.push_back({name, {"interpret"}});
    }
    return IopRig(dq8::test::disc_with(files), specs, "MODULES/IOPRP.IMG", configure);
}

} // namespace

TEST_CASE("iop modules: start runs on its own thread and loadfile answers when it returns", "[iop][kernel]") {
    Basic mod;
    IopRig rig = make_rig({{"BASIC", &mod.b}});
    SifClient& c = rig.client;
    rig.m.trace_mask = rt::trace_bit(rt::TraceCategory::iop);
    c.init_rpc();
    const rt::Cycles before = rig.m.clock().now();
    const auto reply = dq8::test::loadfile_module(c, path_of("BASIC"), std::string_view("a=1\0bb", 6));
    REQUIRE(reply.done);
    CHECK(rig.m.clock().now() - before >= rt::cycles_from_us(1000));
    CHECK(reply.id == 0x20);
    CHECK(reply.start == 0);

    const std::uint32_t out = base_of(rig, "BASIC") + mod.b.offset_of(mod.out);
    CHECK(dq8::test::iop_ram32(*rig.iop, out + 0) == 3);
    CHECK(dq8::test::iop_ram32(*rig.iop, out + 4) == 3);
    const std::string printed = "hello 42 from BASIC, 00be|BAS   |";
    CHECK(dq8::test::iop_ram32(*rig.iop, out + 8) == printed.size() + 1);
    CHECK(dq8::test::iop_ram32(*rig.iop, out + 16) >= 36864); // the system clock after 1 ms

    const rt::MachineEvent* link = last_event(rig.m, "iop_link");
    REQUIRE(link != nullptr);
    CHECK(field(*link, "module") == "BASIC");
    CHECK(field(*link, "hle") == "4");
    CHECK(field(*link, "guest") == "0");
    CHECK(field(*link, "unimplemented") == "0");
    const rt::MachineEvent* module = last_event(rig.m, "iop_module");
    REQUIRE(module != nullptr);
    CHECK(field(*module, "name") == "BASIC");
    CHECK(field(*module, "services") == "interpret");
    CHECK(field(*module, "args") == "a=1 bb");
    CHECK(field(*module, "mode") == "interpret");
    CHECK(field(*module, "base") == std::to_string(base_of(rig, "BASIC")));
    CHECK(field(*module, "start") == "0");
    bool seen = false;
    for (const auto& e : rig.m.emitted) {
        seen = seen || (e.type == "trace" && field(e, "msg") == "printf: " + printed);
    }
    CHECK(seen);

    // Loading it again answers at once with the same id.
    const auto again = dq8::test::loadfile_module(c, path_of("BASIC"));
    REQUIRE(again.done);
    CHECK(again.id == 0x20);
}

TEST_CASE("iop modules: threads run by priority and a signal preempts", "[iop][kernel]") {
    Threads mod;
    IopRig rig = make_rig({{"THREADS", &mod.b}});
    rig.client.init_rpc();
    REQUIRE(dq8::test::loadfile_module(rig.client, path_of("THREADS")).done);
    rig.client.run_until(rig.m.clock().now() + rt::cycles_from_us(200));
    const std::uint32_t log = base_of(rig, "THREADS") + mod.b.offset_of(mod.log);
    CHECK(read_log(rig, log) == std::vector<std::uint32_t>{0x10, 0x11, 2, 3, 4});
    // Both threads returned and are dormant; the start thread is gone.
    rt::IopExec& x = rig.iop->interpreter();
    CHECK(x.thread(1) == nullptr);
    REQUIRE(x.thread(2) != nullptr);
    CHECK(x.thread(2)->status == rt::IopThreadStatus::dormant);
    CHECK(x.thread(3)->status == rt::IopThreadStatus::dormant);
}

TEST_CASE("iop modules: a hardware timer's handler runs at each compare in the interrupt context",
          "[iop][kernel]") {
    Timer mod;
    IopRig rig = make_rig({{"TIMER", &mod.b}});
    rig.client.init_rpc();
    REQUIRE(dq8::test::loadfile_module(rig.client, path_of("TIMER")).done);
    rig.client.run_until(rig.m.clock().now() + rt::cycles_from_us(10'000));
    const std::uint32_t base = base_of(rig, "TIMER");
    CHECK(read_log(rig, base + mod.b.offset_of(mod.log)) ==
          std::vector<std::uint32_t>{0, 0, 0, s32(rt::iop_error::timer_not_inuse), 0});
    CHECK(dq8::test::iop_ram32(*rig.iop, base + mod.b.offset_of(mod.count)) == 5);
    CHECK(dq8::test::iop_ram32(*rig.iop, base + mod.b.offset_of(mod.seen)) == 0x77);
    const std::uint32_t t0 = dq8::test::iop_ram32(*rig.iop, base + mod.b.offset_of(mod.stamps));
    const std::uint32_t t1 = dq8::test::iop_ram32(*rig.iop, base + mod.b.offset_of(mod.stamps) + 8);
    CHECK(t1 - t0 >= 5 * 36864);
    CHECK(t1 - t0 < 5 * 36864 + 2000);
}

TEST_CASE("iop modules: event flags wait for all bits and clear them", "[iop][kernel]") {
    IrxBuilder b("EVENTS");
    const Label log = b.zeros(64);
    const Label evf_param = b.words({0, 0, 0});
    const Label evf = b.zeros(4);
    const Label res = b.zeros(4);
    b.imports("thbase", {4, 6});
    b.imports("thevent", {4, 6, 10});
    const Label waiter = b.label();
    const Label p_waiter = thread_param(b, waiter, 20);
    const Label start = b.here();
    b.enter(32);
    b.la(m::a0, evf_param);
    b.jal(b.stub("thevent", 4));
    b.store(evf, 0, m::v0);
    spawn(b, p_waiter, 0);
    b.load(m::a0, evf);
    b.li(m::a1, 1);
    b.jal(b.stub("thevent", 6));
    b.append_value(log, 0x51);
    b.load(m::a0, evf);
    b.li(m::a1, 2);
    b.jal(b.stub("thevent", 6));
    b.append_value(log, 0x52);
    b.li(m::v0, 0);
    b.leave(32);
    b.bind(waiter);
    b.enter(32);
    b.load(m::a0, evf);
    b.li(m::a1, 3);
    b.li(m::a2, 0x10); // AND, clear the bits waited for
    b.la(m::a3, res);
    b.jal(b.stub("thevent", 10));
    b.append(log, m::v0);
    b.load(m::t0, res);
    b.append(log, m::t0);
    b.load(m::a0, evf);
    b.li(m::a1, 4);
    b.li(m::a2, 1); // OR: waits for good
    b.la(m::a3, res);
    b.jal(b.stub("thevent", 10));
    b.leave(32);
    b.entry(start);

    IopRig rig = make_rig({{"EVENTS", &b}});
    rig.client.init_rpc();
    REQUIRE(dq8::test::loadfile_module(rig.client, path_of("EVENTS")).done);
    rig.client.run_until(rig.m.clock().now() + rt::cycles_from_us(200));
    CHECK(read_log(rig, base_of(rig, "EVENTS") + b.offset_of(log)) ==
          std::vector<std::uint32_t>{0x51, 0x52, 0, 3});
    rt::IopExec& x = rig.iop->interpreter();
    REQUIRE(x.event_flags().size() == 1);
    CHECK(x.event_flags().begin()->second.bits == 0);
    REQUIRE(x.thread(2) != nullptr);
    CHECK(x.thread(2)->status == rt::IopThreadStatus::waiting);
    CHECK(x.thread(2)->wait == rt::IopWait::event_flag);
}

TEST_CASE("iop modules: an alarm repeats with its handler's interval until it returns 0", "[iop][kernel]") {
    IrxBuilder b("ALARMS");
    const Label count = b.zeros(4);
    const Label seen = b.zeros(4);
    const Label tid = b.zeros(4);
    const Label stamps = b.zeros(16);
    const Label clock = b.words({3686, 0});
    b.imports("thbase", {4, 6, 20, 24, 26, 34, 36});
    const Label body = b.label();
    const Label handler = b.label();
    const Label p_body = thread_param(b, body, 20);
    const Label start = b.here();
    b.enter(32);
    spawn(b, p_body, 0);
    b.li(m::v0, 0);
    b.leave(32);
    b.bind(body);
    b.enter(32);
    b.jal(b.stub("thbase", 20));
    b.store(tid, 0, m::v0);
    b.la(m::a0, stamps);
    b.jal(b.stub("thbase", 34));
    b.la(m::a0, clock);
    b.la(m::a1, handler);
    b.li(m::a2, 0x99);
    b.jal(b.stub("thbase", 36));
    b.jal(b.stub("thbase", 24));
    b.la(m::a0, stamps);
    b.emit(m::addiu(m::a0, m::a0, 8));
    b.jal(b.stub("thbase", 34));
    b.leave(32);
    b.bind(handler);
    b.enter(32);
    b.store(seen, 0, m::a0);
    b.load(m::t0, count);
    b.emit(m::addiu(m::t0, m::t0, 1));
    b.store(count, 0, m::t0);
    b.emit(m::slti(m::t1, m::t0, 3));
    const Label again = b.label();
    b.bne(m::t1, m::zero, again);
    b.load(m::a0, tid);
    b.jal(b.stub("thbase", 26));
    b.li(m::v0, 0);
    b.leave(32);
    b.bind(again);
    b.li(m::v0, 3686);
    b.leave(32);
    b.entry(start);

    IopRig rig = make_rig({{"ALARMS", &b}});
    rig.client.init_rpc();
    REQUIRE(dq8::test::loadfile_module(rig.client, path_of("ALARMS")).done);
    rig.client.run_until(rig.m.clock().now() + rt::cycles_from_us(2000));
    const std::uint32_t base = base_of(rig, "ALARMS");
    CHECK(dq8::test::iop_ram32(*rig.iop, base + b.offset_of(count)) == 3);
    CHECK(dq8::test::iop_ram32(*rig.iop, base + b.offset_of(seen)) == 0x99);
    const std::uint32_t t0 = dq8::test::iop_ram32(*rig.iop, base + b.offset_of(stamps));
    const std::uint32_t t1 = dq8::test::iop_ram32(*rig.iop, base + b.offset_of(stamps) + 8);
    CHECK(t1 - t0 >= 3 * 3686);
    CHECK(t1 - t0 < 3 * 3686 + 2000);
    CHECK(rig.iop->interpreter().thread(2)->status == rt::IopThreadStatus::dormant);
}

TEST_CASE("iop modules: sysmem allocates low, high and at an address in 256-byte units", "[iop][kernel]") {
    IrxBuilder b("SYSMEM");
    const Label log = b.zeros(64);
    b.imports("sysmem", {4, 5});
    const Label start = b.here();
    b.enter(32);
    b.call(b.stub("sysmem", 4), {0, 100, 0});
    b.append(log, m::v0);
    b.call(b.stub("sysmem", 4), {1, 100, 0});
    b.append(log, m::v0);
    b.call(b.stub("sysmem", 4), {2, 0x300, 0x00180010});
    b.append(log, m::v0);
    b.call(b.stub("sysmem", 4), {2, 0x100, 0x00180100});
    b.append(log, m::v0);
    b.load(m::a0, log, 4);
    b.jal(b.stub("sysmem", 5));
    b.append(log, m::v0);
    b.load(m::a0, log, 4);
    b.jal(b.stub("sysmem", 5));
    b.append(log, m::v0);
    b.li(m::v0, 0);
    b.leave(32);
    b.entry(start);

    IopRig rig = make_rig({{"SYSMEM", &b}});
    rig.client.init_rpc();
    REQUIRE(dq8::test::loadfile_module(rig.client, path_of("SYSMEM")).done);
    const auto v = read_log(rig, base_of(rig, "SYSMEM") + b.offset_of(log));
    REQUIRE(v.size() == 6);
    CHECK(v[0] != 0);
    CHECK(v[0] % 256 == 0);
    CHECK(v[1] > v[0]);
    CHECK(v[1] % 256 == 0);
    CHECK(v[1] + 0x100 <= rt::IopMemory::heap_end);
    CHECK(v[2] == 0x00180000u);
    CHECK(v[3] == 0);
    CHECK(v[4] == 0);
    CHECK(v[5] == s32(rt::iop_error::error));
}

TEST_CASE("iop modules: an allocation mode sysmem does not have stops", "[iop][kernel]") {
    IrxBuilder b("BADMODE");
    b.imports("sysmem", {4});
    const Label start = b.here();
    b.enter(32);
    b.call(b.stub("sysmem", 4), {3, 100, 0});
    b.leave(32);
    b.entry(start);
    IopRig rig = make_rig({{"BADMODE", &b}});
    rig.client.init_rpc();
    try {
        (void)dq8::test::loadfile_module(rig.client, path_of("BADMODE"));
        FAIL("no stop");
    } catch (const rt::GuestStop& s) {
        CHECK_THAT(s.info().message, ContainsSubstring("AllocSysMemory: allocation mode 3"));
        CHECK_THAT(s.info().message, ContainsSubstring("BADMODE+0x"));
    }
}

TEST_CASE("iop modules: a CPU fault stops with the thread, pc and module offset", "[iop][kernel]") {
    IrxBuilder b("FAULT");
    const Label start = b.here();
    b.emit(m::nop());
    b.emit(m::break_(3));
    b.ret();
    b.entry(start);
    IopRig rig = make_rig({{"FAULT", &b}});
    rig.client.init_rpc();
    try {
        (void)dq8::test::loadfile_module(rig.client, path_of("FAULT"));
        FAIL("no stop");
    } catch (const rt::GuestStop& s) {
        CHECK(s.info().reason == rt::StopReason::guest_exception);
        CHECK(s.info().number == 9);
        CHECK_THAT(s.info().message, ContainsSubstring("a break instruction"));
        CHECK_THAT(s.info().message, ContainsSubstring("IOP thread 1 (FAULT)"));
        CHECK_THAT(s.info().message, ContainsSubstring("(FAULT+0x00000004)"));
    }
}

TEST_CASE("iop modules: the same program twice gives the same events and IOP RAM", "[iop][kernel]") {
    const auto run = [] {
        Threads threads;
        Timer timer;
        IopRig rig = make_rig({{"THREADS", &threads.b}, {"TIMER", &timer.b}});
        rig.client.init_rpc();
        REQUIRE(dq8::test::loadfile_module(rig.client, path_of("THREADS")).done);
        REQUIRE(dq8::test::loadfile_module(rig.client, path_of("TIMER")).done);
        rig.client.run_until(rig.m.clock().now() + rt::cycles_from_us(8000));
        std::vector<std::string> events;
        for (const auto& e : rig.m.emitted) {
            std::string line = e.type;
            for (const auto& [k, v] : e.fields) {
                line += " " + k + "=" + field(e, k);
            }
            events.push_back(line);
        }
        const auto ram = rig.iop->ram();
        return std::make_tuple(events, std::vector<std::uint8_t>(ram.begin(), ram.end()), rig.m.clock().now(),
                               rig.iop->interpreter().instructions());
    };
    const auto a = run();
    const auto b = run();
    CHECK(std::get<0>(a) == std::get<0>(b));
    CHECK(std::get<1>(a) == std::get<1>(b));
    CHECK(std::get<2>(a) == std::get<2>(b));
    CHECK(std::get<3>(a) == std::get<3>(b));
    CHECK(std::get<3>(a) > 0);
}

TEST_CASE("iop modules: with no interpreted module nothing IOP-side is ever scheduled", "[iop][kernel]") {
    IopRig rig; // the native module table only
    SifClient& c = rig.client;
    c.init_rpc();
    REQUIRE(dq8::test::loadfile_module(c, "cdrom0:\\MODULES\\SDRDRV.IRX;1").done);
    REQUIRE(dq8::test::loadfile_module(c, "cdrom0:\\MODULES\\PADMAN.IRX;1").done);
    c.run_until(rig.m.clock().now() + rt::cycles_from_us(5000));
    for (const auto& e : rig.m.events().pending()) {
        CHECK(e.tag.substr(0, 4) != "iop_");
    }
    rt::IopExec& x = rig.iop->interpreter();
    CHECK(x.slices() == 0);
    CHECK(x.instructions() == 0);
    CHECK_FALSE(x.has_modules());
    const rt::MachineEvent* module = last_event(rig.m, "iop_module");
    REQUIRE(module != nullptr);
    CHECK(field(*module, "mode") == "native");
}

TEST_CASE("iop modules: an IOP reboot forgets every interpreted module and its events", "[iop][kernel]") {
    Timer mod;
    IopRig rig = make_rig({{"TIMER", &mod.b}});
    SifClient& c = rig.client;
    c.init_rpc();
    REQUIRE(dq8::test::loadfile_module(c, path_of("TIMER")).done);
    c.run_until(rig.m.clock().now() + rt::cycles_from_us(1500)); // the timer is running
    REQUIRE(c.reset_iop("cdrom0:\\MODULES\\IOPRP.IMG;1") != 0);
    REQUIRE(c.sync_iop());
    rt::IopExec& x = rig.iop->interpreter();
    CHECK_FALSE(x.has_modules());
    CHECK(x.thread(2) == nullptr);
    for (const auto& e : rig.m.events().pending()) {
        CHECK(e.tag.substr(0, 4) != "iop_");
    }
    // And it loads again from scratch (the loader binds anew).
    c.init_rpc();
    c.write32(0x00012F00 + 0x24, 0);
    const auto reply = dq8::test::loadfile_module(c, path_of("TIMER"));
    REQUIRE(reply.done);
    CHECK(reply.id == 0x20);
}

TEST_CASE("iop hle: every census ordinal is implemented", "[iop][kernel]") {
    std::map<std::pair<std::string_view, std::uint16_t>, const rt::HleFunction*> implemented;
    for (const rt::HleFunction& f : rt::hle_functions()) {
        CHECK(implemented.emplace(std::pair{f.library, f.ordinal}, &f).second);
    }
    std::set<std::string_view> libraries;
    std::size_t covered = 0;
    for (const auto& [library, ordinal] : rt::sound_module_census()) {
        libraries.insert(library);
        const auto it = implemented.find({library, ordinal});
        INFO(library << " " << ordinal);
        REQUIRE(it != implemented.end());
        CHECK(it->second->fn != nullptr);
        CHECK_FALSE(it->second->name.empty());
        ++covered;
    }
    CHECK(covered == 66);
    CHECK(libraries.size() == 11);
    // Nothing beyond the census is claimed: an ordinal outside it links to a stop trap (test_iop_sif.cpp).
    CHECK(implemented.size() == 66);
}

TEST_CASE("iop printf: the conversions IOP modules use", "[iop][kernel]") {
    std::vector<std::uint32_t> words;
    std::map<std::uint32_t, std::string> strings{{0x100, "abc"}, {0x200, "longer text"}};
    rt::IopPrintfSource src;
    src.word = [&words](unsigned slot) { return slot - 1 < words.size() ? words[slot - 1] : 0u; };
    src.string_at = [&strings](std::uint32_t p) { return strings[p]; };
    const auto fmt = [&](std::string_view f, std::vector<std::uint32_t> w) {
        words = std::move(w);
        return rt::iop_format(f, src, 1);
    };
    CHECK(fmt("%d %i %u", {s32(-12), 7, 0xFFFFFFFFu}) == "-12 7 4294967295");
    CHECK(fmt("%x %X %08x %#x %o %#o", {0xBEEF, 0xBEEF, 0x12, 0x12, 8, 8}) ==
          "beef BEEF 00000012 0x12 10 010");
    CHECK(fmt("[%5d][%-5d][%05d][%+d][% d]", {42, 42, s32(-42), 3, 3}) == "[   42][42   ][-0042][+3][ 3]");
    CHECK(fmt("%s|%.4s|%8s|%-8s|", {0x100, 0x200, 0x100, 0x100}) == "abc|long|     abc|abc     |");
    CHECK(fmt("%c%c %%", {'o', 'k'}) == "ok %");
    CHECK(fmt("%*d|%.*s", {4, 9, 2, 0x200}) == "   9|lo");
    CHECK(fmt("%p %s", {0x1234, 0}) == "0x1234 (null)");
    CHECK(fmt("%hd %hhu %ld", {0x12345, 0x1FF, 5}) == "9029 255 5");
    // A 64-bit argument takes an even-aligned pair: slot 1 is skipped after the format in slot 0.
    CHECK(fmt("%lld", {0, 0xFFFFFFFEu, 0xFFFFFFFFu}) == "-2");
    CHECK(fmt("%q %", {}) == "%q %");
}

TEST_CASE("iop hle: status queries, error returns and the C library", "[iop][kernel]") {
    IrxBuilder b("MISC");
    const Label log = b.zeros(256);
    const Label info = b.zeros(0x40);
    const Label state = b.zeros(8);
    const Label clk = b.words({0, 0});
    const Label clk2 = b.words({92'160'000u, 0}); // 2.5 s of the system clock
    const Label sec = b.zeros(8);
    const Label buf = b.zeros(16);
    const Label buf2 = b.zeros(16);
    const Label num = b.string("  -0x1F");
    const Label oct = b.string("777");
    const Label end = b.zeros(4);
    const Label sema_param = b.words({0, 0, 1, 1});
    const Label evf_param = b.words({0, 0, 0xF0});
    const Label far = b.words({0x10000000u, 0});
    const Label res = b.zeros(4);
    b.imports("thbase", {4, 5, 6, 10, 14, 20, 22, 28, 36, 37, 38, 39, 40});
    b.imports("intrman", {4, 5, 6, 7, 17, 18, 23});
    b.imports("sysclib", {8, 12, 14, 17, 27, 29, 36});
    b.imports("thsemap", {4, 5, 6, 8});
    b.imports("thevent", {4, 5, 8, 10});
    b.imports("timrman", {4, 6, 24});
    b.imports("loadcore", {7});
    const Label idle = b.label();
    const Label handler = b.label();
    const Label p_idle = thread_param(b, idle, 50);
    const auto logged = [&](std::string_view lib, std::uint16_t ord,
                            std::initializer_list<std::uint32_t> args) {
        b.call(b.stub(lib, ord), args);
        b.append(log, m::v0);
    };
    const auto logged_word = [&](Label at, std::uint32_t off) {
        b.load(m::t0, at, off);
        b.append(log, m::t0);
    };
    const auto la_call = [&](std::string_view lib, std::uint16_t ord, Label a0, std::uint32_t a1 = 0,
                             std::uint32_t a2 = 0, std::uint32_t a3 = 0) {
        b.la(m::a0, a0);
        b.li(m::a1, a1);
        b.li(m::a2, a2);
        b.li(m::a3, a3);
        b.jal(b.stub(lib, ord));
        b.append(log, m::v0);
    };

    const Label start = b.here();
    b.enter(48);
    // Threads: create, query, start, re-prioritise, terminate, delete.
    b.la(m::a0, p_idle);
    b.jal(b.stub("thbase", 4));
    b.emit(m::move(m::s0, m::v0));
    b.emit(m::move(m::a0, m::s0));
    b.la(m::a1, info);
    b.jal(b.stub("thbase", 22));
    b.append(log, m::v0);  // 0
    logged_word(info, 8);  // dormant 0x10
    logged_word(info, 28); // initial priority 50
    b.emit(m::move(m::a0, m::s0));
    b.li(m::a1, 40);
    b.jal(b.stub("thbase", 14));
    b.append(log, m::v0); // dormant: -413
    b.emit(m::move(m::a0, m::s0));
    b.li(m::a1, 0);
    b.jal(b.stub("thbase", 6));
    b.append(log, m::v0); // 0
    b.emit(m::move(m::a0, m::s0));
    b.li(m::a1, 40);
    b.jal(b.stub("thbase", 14));
    b.append(log, m::v0); // 0
    b.emit(m::move(m::a0, m::s0));
    b.la(m::a1, info);
    b.jal(b.stub("thbase", 22));
    logged_word(info, 8);  // ready 2
    logged_word(info, 32); // current priority 40
    b.emit(m::move(m::a0, m::s0));
    b.jal(b.stub("thbase", 10));
    b.append(log, m::v0); // 0
    b.emit(m::move(m::a0, m::s0));
    b.jal(b.stub("thbase", 5));
    b.append(log, m::v0); // 0
    b.emit(m::move(m::a0, m::s0));
    b.jal(b.stub("thbase", 5));
    b.append(log, m::v0);          // -407
    logged("thbase", 10, {0});     // self: -406
    logged("thbase", 28, {0});     // wakeup count 0
    logged("thbase", 20, {});      // thread id 1
    logged("intrman", 23, {});     // not an interrupt: 0
    la_call("intrman", 17, state); // 0
    logged_word(state, 0);         // 1
    la_call("intrman", 17, state); // -102
    logged("intrman", 18, {1});    // 0
    // Time conversions.
    b.li(m::a0, 1000);
    b.la(m::a1, clk);
    b.jal(b.stub("thbase", 39));
    logged_word(clk, 0); // 36864
    b.la(m::a0, clk2);
    b.la(m::a1, sec);
    b.la(m::a2, sec);
    b.emit(m::addiu(m::a2, m::a2, 4));
    b.jal(b.stub("thbase", 40));
    logged_word(sec, 0); // 2
    logged_word(sec, 4); // 500000
    // sysclib.
    la_call("sysclib", 14, buf, 0x41, 5); // memset returns buf
    la_call("sysclib", 27, buf);          // 5
    b.la(m::a0, buf2);
    b.la(m::a1, buf);
    b.li(m::a2, 3);
    b.jal(b.stub("sysclib", 12));
    b.la(m::a0, buf);
    b.la(m::a1, buf2);
    b.li(m::a2, 3);
    b.jal(b.stub("sysclib", 29));
    b.append(log, m::v0); // 0
    b.la(m::a0, buf);
    b.la(m::a1, buf2);
    b.li(m::a2, 4);
    b.jal(b.stub("sysclib", 29));
    b.append(log, m::v0); // 'A' - 0 = 65
    b.la(m::a0, buf);
    b.li(m::a1, 2);
    b.jal(b.stub("sysclib", 17)); // bzero returns nothing
    la_call("sysclib", 27, buf);  // 0
    logged("sysclib", 8, {'a'});  // lower | hex: 0x42
    logged("sysclib", 8, {' '});  // space | blank: 0x88
    b.la(m::a0, num);
    b.la(m::a1, end);
    b.li(m::a2, 0);
    b.jal(b.stub("sysclib", 36));
    b.append(log, m::v0); // -31
    b.load(m::t0, end);
    b.la(m::t1, num);
    b.emit(m::subu(m::t0, m::t0, m::t1));
    b.append(log, m::t0); // 7
    b.la(m::a0, oct);
    b.li(m::a1, 0);
    b.li(m::a2, 8);
    b.jal(b.stub("sysclib", 36));
    b.append(log, m::v0); // 511
    // Semaphores.
    b.la(m::a0, sema_param);
    b.jal(b.stub("thsemap", 4));
    b.emit(m::move(m::s1, m::v0));
    b.emit(m::move(m::a0, m::s1));
    b.jal(b.stub("thsemap", 6));
    b.append(log, m::v0); // overflow: -420
    b.emit(m::move(m::a0, m::s1));
    b.jal(b.stub("thsemap", 8));
    b.append(log, m::v0); // 0
    b.emit(m::move(m::a0, m::s1));
    b.jal(b.stub("thsemap", 5));
    b.append(log, m::v0); // 0
    b.emit(m::move(m::a0, m::s1));
    b.jal(b.stub("thsemap", 8));
    b.append(log, m::v0); // -408
    // Event flags.
    b.la(m::a0, evf_param);
    b.jal(b.stub("thevent", 4));
    b.emit(m::move(m::s1, m::v0));
    b.emit(m::move(m::a0, m::s1));
    b.li(m::a1, 0x30);
    b.jal(b.stub("thevent", 8));
    b.emit(m::move(m::a0, m::s1));
    b.li(m::a1, 0x10);
    b.li(m::a2, 1);
    b.la(m::a3, res);
    b.jal(b.stub("thevent", 10));
    b.append(log, m::v0); // 0
    logged_word(res, 0);  // 0x30
    b.emit(m::move(m::a0, m::s1));
    b.li(m::a1, 0);
    b.li(m::a2, 0);
    b.li(m::a3, 0);
    b.jal(b.stub("thevent", 10));
    b.append(log, m::v0); // -423
    b.emit(m::move(m::a0, m::s1));
    b.jal(b.stub("thevent", 5));
    b.append(log, m::v0); // 0
    // Alarms.
    b.la(m::a0, handler);
    b.li(m::a1, 5);
    b.jal(b.stub("thbase", 37));
    b.append(log, m::v0); // -105
    b.la(m::a0, far);
    b.la(m::a1, handler);
    b.li(m::a2, 5);
    b.jal(b.stub("thbase", 36));
    b.la(m::a0, handler);
    b.li(m::a1, 5);
    b.jal(b.stub("thbase", 38));
    b.append(log, m::v0); // 0
    // Interrupt handlers.
    b.li(m::a0, 9);
    b.li(m::a1, 1);
    b.la(m::a2, handler);
    b.li(m::a3, 0);
    b.jal(b.stub("intrman", 4));
    b.append(log, m::v0); // 0
    b.li(m::a0, 9);
    b.li(m::a1, 1);
    b.la(m::a2, handler);
    b.li(m::a3, 0);
    b.jal(b.stub("intrman", 4));
    b.append(log, m::v0);      // -104
    logged("intrman", 6, {9}); // 0
    b.li(m::a0, 9);
    b.la(m::a1, res);
    b.jal(b.stub("intrman", 7));
    b.append(log, m::v0);                  // 0
    logged_word(res, 0);                   // 9
    logged("intrman", 7, {9, 0});          // -103
    logged("intrman", 5, {9});             // 0
    logged("intrman", 5, {9});             // -105
    logged("intrman", 4, {0x40, 1, 0, 0}); // -101
    // Timers.
    logged("timrman", 4, {1, 16, 1}); // an id
    b.emit(m::move(m::s1, m::v0));
    b.emit(m::move(m::a0, m::s1));
    b.jal(b.stub("timrman", 6));
    b.append(log, m::v0); // 0
    b.emit(m::move(m::a0, m::s1));
    b.jal(b.stub("timrman", 6));
    b.append(log, m::v0);                // -151
    logged("timrman", 24, {0x12345678}); // -151
    logged("timrman", 4, {2, 32, 1});    // -150
    logged("loadcore", 7, {0x00100000}); // -213
    b.li(m::v0, 0);
    b.leave(48);
    b.bind(idle);
    b.ret();
    b.bind(handler);
    b.li(m::v0, 0);
    b.ret();
    b.entry(start);

    IopRig rig = make_rig({{"MISC", &b}});
    rig.client.init_rpc();
    REQUIRE(dq8::test::loadfile_module(rig.client, path_of("MISC")).done);
    const std::uint32_t base = base_of(rig, "MISC");
    const auto v = read_log(rig, base + b.offset_of(log));
    const std::uint32_t buf_at = base + b.offset_of(buf);
    const std::uint32_t timer_id = (3u << 28) | (0xBF801120u >> 4);
    const std::vector<std::uint32_t> expected{
        0,         0x10,      50,        s32(-413), 0,         0,         2,        40,        0,
        0,         s32(-407), s32(-406), 0,         1,         0,         0,        1,         s32(-102),
        0,         36864,     2,         500000,    buf_at,    5,         0,        65,        0,
        0x42,      0x88,      s32(-31),  7,         511,       s32(-420), 0,        0,         s32(-408),
        0,         0x30,      s32(-423), 0,         s32(-105), 0,         0,        s32(-104), 0,
        0,         9,         s32(-103), 0,         s32(-105), s32(-101), timer_id, 0,         s32(-151),
        s32(-151), s32(-150), s32(-213)};
    REQUIRE(v.size() == expected.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        INFO("log entry " << i);
        CHECK(v[i] == expected[i]);
    }
}
