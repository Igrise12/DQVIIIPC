// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_stop.h"
#include "runtime/iop/iop_exec.h"
#include "runtime/iop/iop_modules.h"
#include "support/irx_builder.h"
#include "support/sif_client.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace rt = dq8::rt;
namespace m = dq8::test::mips;
namespace ia = dq8::test::iopasm;
using Catch::Matchers::ContainsSubstring;
using dq8::test::IopRig;
using dq8::test::IrxBuilder;
using dq8::test::SifClient;
using Label = IrxBuilder::Label;

namespace {

Label thread_param(IrxBuilder& b, Label entry, std::uint32_t priority) {
    const Label p = b.words({0, 0, 0, 0x800, priority});
    b.pointer(p, 8, entry);
    return p;
}

void spawn(IrxBuilder& b, Label param, std::uint32_t arg) {
    b.la(m::a0, param);
    b.jal(b.stub("thbase", 4));
    b.emit(m::move(m::a0, m::v0));
    b.li(m::a1, arg);
    b.jal(b.stub("thbase", 6));
}

// A start function that spawns `body` at `priority` and returns 0.
void start_spawning(IrxBuilder& b, Label body, std::uint32_t priority) {
    const Label p = thread_param(b, body, priority);
    const Label start = b.here();
    b.enter(32);
    spawn(b, p, 0);
    b.li(m::v0, 0);
    b.leave(32);
    b.entry(start);
}

std::string path_of(const std::string& name) {
    return "cdrom0:\\MODULES\\" + name + ".IRX;1";
}

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

std::uint32_t base_of(IopRig& rig, std::string_view name) {
    for (const auto& mod : rig.iop->interpreter().modules()) {
        if (mod.name == name) {
            return mod.base;
        }
    }
    throw std::logic_error("module " + std::string(name) + " is not loaded");
}

std::uint32_t ram32(IopRig& rig, std::uint32_t address) {
    return dq8::test::iop_ram32(*rig.iop, address);
}

std::vector<std::uint8_t> words(std::initializer_list<std::uint32_t> values) {
    std::vector<std::uint8_t> out;
    for (const std::uint32_t v : values) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
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

template <typename T> T get(const rt::MachineEvent& e, std::string_view key) {
    for (const auto& [k, v] : e.fields) {
        if (k == key) {
            return std::get<T>(v);
        }
    }
    throw std::logic_error("no field " + std::string(key));
}

// A server for 0x7E57 on its own thread: the reply is the sum of the argument words, the function number and
// the argument size.
struct RpcServer {
    IrxBuilder b{"RPCSRV"};
    Label sd;
    Label buf;
    RpcServer() {
        const Label qd = b.zeros(0x18);
        sd = b.zeros(0x44);
        buf = b.zeros(0x40);
        const Label reply = b.zeros(16);
        b.imports("thbase", {4, 6, 20});
        b.imports("sifcmd", {14, 17, 19, 22});
        const Label body = b.label();
        const Label func = b.label();
        start_spawning(b, body, 30);

        b.bind(body);
        b.enter(48);
        b.call(b.stub("sifcmd", 14), {0});
        b.jal(b.stub("thbase", 20));
        b.emit(m::move(m::a1, m::v0));
        b.la(m::a0, qd);
        b.jal(b.stub("sifcmd", 19));
        b.la(m::a0, sd);
        b.li(m::a1, 0x7E57);
        b.la(m::a2, func);
        b.la(m::a3, buf);
        b.emit(m::sw(m::zero, 16, m::sp));
        b.emit(m::sw(m::zero, 20, m::sp));
        b.la(m::t0, qd);
        b.emit(m::sw(m::t0, 24, m::sp));
        b.jal(b.stub("sifcmd", 17));
        b.la(m::a0, qd);
        b.jal(b.stub("sifcmd", 22));
        b.leave(48);

        b.bind(func);
        b.emit(m::addu(m::t3, m::zero, m::zero));
        b.emit(m::move(m::t0, m::a1));
        b.emit(ia::srl(m::t1, m::a2, 2));
        const Label loop = b.here();
        const Label done = b.label();
        b.beq(m::t1, m::zero, done);
        b.emit(m::lw(m::t2, 0, m::t0));
        b.emit(m::nop());
        b.emit(m::addu(m::t3, m::t3, m::t2));
        b.emit(m::addiu(m::t0, m::t0, 4));
        b.emit(m::addiu(m::t1, m::t1, -1));
        b.b(loop);
        b.bind(done);
        b.la(m::t9, reply);
        b.emit(m::sw(m::t3, 0, m::t9));
        b.emit(m::sw(m::a0, 4, m::t9));
        b.emit(m::sw(m::a2, 8, m::t9));
        b.emit(m::move(m::v0, m::t9));
        b.ret();
    }
};

} // namespace

TEST_CASE("iop sif: an interpreted module's RPC server answers the EE", "[iop][sif]") {
    RpcServer mod;
    IopRig rig = make_rig({{"RPCSRV", &mod.b}});
    SifClient& c = rig.client;
    c.init_rpc();
    REQUIRE(dq8::test::loadfile_module(c, path_of("RPCSRV")).done);
    const std::uint32_t client = 0x00012000;
    std::uint32_t server = 0;
    for (int i = 0; i < 20 && server == 0; ++i) {
        server = c.bind(client, 0x7E57);
        if (server == 0) {
            c.run_until(rig.m.clock().now() + rt::cycles_from_us(100));
        }
    }
    const std::uint32_t base = base_of(rig, "RPCSRV");
    REQUIRE(server == base + mod.b.offset_of(mod.sd));
    CHECK(c.read32(client + 0x14) == base + mod.b.offset_of(mod.buf));
    const rt::MachineEvent* reg = last_event(rig.m, "iop_rpc_register");
    REQUIRE(reg != nullptr);
    CHECK(get<std::uint64_t>(*reg, "sid") == 0x7E57);
    CHECK(get<std::string>(*reg, "module") == "RPCSRV");

    c.write(0x00030000, words({1, 2, 3, 4}));
    REQUIRE(c.call(client, 5, 0x00030000, 16, 0x00032000, 12));
    CHECK(c.read(0x00032000, 12) == words({10, 5, 16}));
    c.write(0x00030000, words({7, 8}));
    REQUIRE(c.call(client, 6, 0x00030000, 8, 0x00032000, 12));
    CHECK(c.read(0x00032000, 12) == words({15, 6, 8}));
    // Without an end packet the reply still lands and the call ends.
    c.write(0x00030000, words({100}));
    REQUIRE(c.call(client, 7, 0x00030000, 4, 0x00032000, 12, true, false));
    CHECK(c.read(0x00032000, 12) == words({100, 7, 4}));
    // The server structure holds what the IOP library fills in.
    CHECK(ram32(rig, server + 0x00) == 0x7E57);
    CHECK(ram32(rig, server + 0x24) == 7);
    CHECK(rig.iop->interpreter().thread(2)->wait == rt::IopWait::rpc_server);
}

TEST_CASE("iop sif: an interpreted module binds and calls an EE server", "[iop][sif]") {
    IrxBuilder b("RPCCLI");
    const Label cd = b.zeros(0x28);
    const Label send = b.words({5, 6, 7, 8});
    const Label recv = b.zeros(16);
    const Label recv2 = b.zeros(16);
    const Label out = b.zeros(16);
    const Label tid = b.zeros(4);
    b.imports("thbase", {4, 6, 20, 24, 26, 33});
    b.imports("sifcmd", {15, 16});
    const Label body = b.label();
    const Label endfn = b.label();
    start_spawning(b, body, 30);

    b.bind(body);
    b.enter(48);
    b.jal(b.stub("thbase", 20));
    b.store(tid, 0, m::v0);
    const Label bind_again = b.here();
    b.la(m::a0, cd);
    b.li(m::a1, 0x6001);
    b.li(m::a2, 0);
    b.jal(b.stub("sifcmd", 15));
    b.load(m::t0, cd, 0x24);
    const Label bound = b.label();
    b.bne(m::t0, m::zero, bound);
    b.call(b.stub("thbase", 33), {1000});
    b.b(bind_again);
    b.bind(bound);
    b.la(m::a0, cd);
    b.li(m::a1, 9);
    b.li(m::a2, 0);
    b.la(m::a3, send);
    b.li(m::t0, 16);
    b.emit(m::sw(m::t0, 16, m::sp));
    b.la(m::t0, recv);
    b.emit(m::sw(m::t0, 20, m::sp));
    b.li(m::t0, 16);
    b.emit(m::sw(m::t0, 24, m::sp));
    b.emit(m::sw(m::zero, 28, m::sp));
    b.emit(m::sw(m::zero, 32, m::sp));
    b.jal(b.stub("sifcmd", 16));
    b.store(out, 0, m::v0);
    b.li(m::t0, 1);
    b.store(out, 4, m::t0);
    // No wait, with an end function that wakes this thread.
    b.la(m::a0, cd);
    b.li(m::a1, 10);
    b.li(m::a2, 1);
    b.la(m::a3, send);
    b.li(m::t0, 8);
    b.emit(m::sw(m::t0, 16, m::sp));
    b.la(m::t0, recv2);
    b.emit(m::sw(m::t0, 20, m::sp));
    b.li(m::t0, 8);
    b.emit(m::sw(m::t0, 24, m::sp));
    b.la(m::t0, endfn);
    b.emit(m::sw(m::t0, 28, m::sp));
    b.li(m::t0, 0x55);
    b.emit(m::sw(m::t0, 32, m::sp));
    b.jal(b.stub("sifcmd", 16));
    b.jal(b.stub("thbase", 24));
    b.li(m::t0, 2);
    b.store(out, 12, m::t0);
    b.leave(48);

    b.bind(endfn);
    b.enter(32);
    b.store(out, 8, m::a0);
    b.load(m::a0, tid);
    b.jal(b.stub("thbase", 26));
    b.leave(32);

    IopRig rig = make_rig({{"RPCCLI", &b}});
    SifClient& c = rig.client;
    c.add_ee_server(0x6001, 0x00015000, 0x00015100, [](std::uint32_t fno, std::vector<std::uint8_t> args) {
        for (std::size_t i = 0; i + 4 <= args.size(); i += 4) {
            std::uint32_t v = 0;
            std::memcpy(&v, args.data() + i, 4);
            v = fno == 9 ? v * 2 : v + 100;
            std::memcpy(args.data() + i, &v, 4);
        }
        return args;
    });
    c.init_rpc();
    REQUIRE(dq8::test::loadfile_module(c, path_of("RPCCLI")).done);
    c.run_until(rig.m.clock().now() + rt::cycles_from_us(20'000));
    const std::uint32_t base = base_of(rig, "RPCCLI");
    const auto at = [&](Label l, std::uint32_t off = 0) { return ram32(rig, base + b.offset_of(l) + off); };
    CHECK(at(recv, 0) == 10);
    CHECK(at(recv, 12) == 16);
    CHECK(at(recv2, 0) == 105);
    CHECK(at(recv2, 4) == 106);
    CHECK(at(recv2, 8) == 0);
    CHECK(at(out, 0) == 0);
    CHECK(at(out, 4) == 1);
    CHECK(at(out, 8) == 0x55);
    CHECK(at(out, 12) == 2);
    CHECK(at(cd, 0x24) == 0x00015000u);
    CHECK(at(cd, 0x14) == 0x00015100u);
    CHECK(c.ee_calls(0x6001) == 2);
    CHECK(c.ee_binds() == 1);
}

TEST_CASE("iop sif: sceSifSetDma copies IOP data to the EE and sceSifDmaStat follows it", "[iop][sif]") {
    IrxBuilder b("SIFDMA");
    const Label src = b.words({0x11, 0x22, 0x33, 0x44});
    // 32 KiB (the source and what follows it): long enough to be in flight at the first sceSifDmaStat.
    const Label desc = b.words({0, 0x00016000, 0x8000, 0});
    b.pointer(desc, 0, src);
    const Label log = b.zeros(32);
    b.imports("sifman", {7, 8});
    b.imports("thbase", {33});
    const Label start = b.here();
    b.enter(32);
    b.la(m::a0, desc);
    b.li(m::a1, 1);
    b.jal(b.stub("sifman", 7));
    b.emit(m::move(m::s0, m::v0));
    b.append(log, m::v0);
    b.emit(m::move(m::a0, m::s0));
    b.jal(b.stub("sifman", 8));
    b.append(log, m::v0);
    const Label wait = b.here();
    b.call(b.stub("thbase", 33), {100});
    b.emit(m::move(m::a0, m::s0));
    b.jal(b.stub("sifman", 8));
    b.emit(m::slti(m::t0, m::v0, 0));
    b.beq(m::t0, m::zero, wait);
    b.append(log, m::v0);
    b.li(m::v0, 0);
    b.leave(32);
    b.entry(start);

    IopRig rig = make_rig({{"SIFDMA", &b}});
    SifClient& c = rig.client;
    c.init_rpc();
    REQUIRE(dq8::test::loadfile_module(c, path_of("SIFDMA")).done);
    CHECK(c.read(0x00016000, 16) == words({0x11, 0x22, 0x33, 0x44}));
    const std::uint32_t log_at = base_of(rig, "SIFDMA") + b.offset_of(log);
    CHECK(ram32(rig, log_at) == 3);
    CHECK(ram32(rig, log_at + 4) != 0);
    CHECK(ram32(rig, log_at + 8) == 1);
    CHECK(ram32(rig, log_at + 12) == 0xFFFFFFFFu);
}

TEST_CASE("iop sif: modules link to each other through registered export tables", "[iop][sif]") {
    IrxBuilder lib("LIBA");
    lib.imports("loadcore", {6});
    const Label twice = lib.label();
    const Label lib_start = lib.label();
    const Label table = lib.exports("testlib", {lib_start, lib_start, lib_start, lib_start, twice});
    lib.bind(lib_start);
    lib.enter(32);
    lib.la(m::a0, table);
    lib.jal(lib.stub("loadcore", 6));
    lib.leave(32); // returns RegisterLibraryEntries' result: 0
    lib.bind(twice);
    lib.emit(m::jr(m::ra));
    lib.emit(m::addu(m::v0, m::a0, m::a0));
    lib.entry(lib_start);

    IrxBuilder user("USEB");
    const Label out = user.zeros(4);
    user.imports("testlib", {4});
    const Label start = user.here();
    user.enter(32);
    user.call(user.stub("testlib", 4), {21});
    user.store(out, 0, m::v0);
    user.li(m::v0, 0);
    user.leave(32);
    user.entry(start);

    SECTION("in order") {
        IopRig rig = make_rig({{"LIBA", &lib}, {"USEB", &user}});
        rig.client.init_rpc();
        REQUIRE(dq8::test::loadfile_module(rig.client, path_of("LIBA")).done);
        REQUIRE(dq8::test::loadfile_module(rig.client, path_of("USEB")).done);
        CHECK(ram32(rig, base_of(rig, "USEB") + user.offset_of(out)) == 42);
        const rt::MachineEvent* link = last_event(rig.m, "iop_link");
        REQUIRE(link != nullptr);
        CHECK(get<std::string>(*link, "module") == "USEB");
        CHECK(get<std::uint64_t>(*link, "guest") == 1);
        CHECK(get<std::uint64_t>(*link, "hle") == 0);
    }
    SECTION("the user first: the library is missing") {
        IopRig rig = make_rig({{"LIBA", &lib}, {"USEB", &user}});
        rig.client.init_rpc();
        try {
            (void)dq8::test::loadfile_module(rig.client, path_of("USEB"));
            FAIL("no stop");
        } catch (const rt::GuestStop& s) {
            CHECK(s.info().reason == rt::StopReason::unsupported);
            CHECK_THAT(s.info().message, ContainsSubstring("USEB imports library testlib"));
        }
    }
}

TEST_CASE("iop sif: an import nobody implemented links to a stop trap", "[iop][sif]") {
    IrxBuilder b("UNIMPL");
    b.imports("thbase", {25, 20});
    const Label start = b.here();
    b.enter(32);
    b.jal(b.stub("thbase", 20));
    b.call(b.stub("thbase", 25), {1});
    b.li(m::v0, 0);
    b.leave(32);
    b.entry(start);

    SECTION("linked, then stopped at the call") {
        IopRig rig = make_rig({{"UNIMPL", &b}});
        rig.client.init_rpc();
        try {
            (void)dq8::test::loadfile_module(rig.client, path_of("UNIMPL"));
            FAIL("no stop");
        } catch (const rt::GuestStop& s) {
            CHECK(s.info().reason == rt::StopReason::unsupported);
            CHECK_THAT(s.info().message, ContainsSubstring("UNIMPL called thbase ordinal 25"));
            CHECK_THAT(s.info().message, ContainsSubstring("IOP thread 1 (UNIMPL)"));
            CHECK_THAT(s.info().message, ContainsSubstring("call at UNIMPL+0x"));
        }
        const rt::MachineEvent* e = last_event(rig.m, "iop_import_unimplemented");
        REQUIRE(e != nullptr);
        CHECK(get<std::string>(*e, "module") == "UNIMPL");
        CHECK(get<std::string>(*e, "library") == "thbase");
        CHECK(get<std::uint64_t>(*e, "ordinal") == 25);
        const rt::MachineEvent* link = last_event(rig.m, "iop_link");
        REQUIRE(link != nullptr);
        CHECK(get<std::uint64_t>(*link, "hle") == 1);
        CHECK(get<std::uint64_t>(*link, "unimplemented") == 1);
    }
    SECTION("strict imports stop at load") {
        IopRig rig = make_rig({{"UNIMPL", &b}}, [](rt::IopConfig& cfg) { cfg.strict_imports = true; });
        rig.client.init_rpc();
        try {
            (void)dq8::test::loadfile_module(rig.client, path_of("UNIMPL"));
            FAIL("no stop");
        } catch (const rt::GuestStop& s) {
            CHECK_THAT(s.info().message,
                       ContainsSubstring("UNIMPL imports HLE functions nobody implemented: thbase 25"));
        }
        CHECK(rig.iop->interpreter().instructions() == 0);
    }
}

TEST_CASE("iop sif: a module file that is not an IRX stops the load", "[iop][sif]") {
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files{
        {"BROKEN", std::vector<std::uint8_t>(64, 7)}};
    std::vector<rt::IopModuleSpec> specs = dq8::test::interpreted({"BROKEN"});
    IopRig rig(dq8::test::disc_with(files), specs);
    rig.client.init_rpc();
    try {
        (void)dq8::test::loadfile_module(rig.client, path_of("BROKEN"));
        FAIL("no stop");
    } catch (const rt::GuestStop& s) {
        CHECK_THAT(s.info().message, ContainsSubstring("loading BROKEN"));
        CHECK_THAT(s.info().message, ContainsSubstring("not an ELF"));
    }
}

TEST_CASE("iop sif: the interpret service stands alone in its row", "[iop][sif]") {
    std::vector<rt::IopModuleSpec> specs = dq8::test::test_modules();
    specs.push_back({"MIXED", {"interpret", "null:0x1234"}});
    CHECK_THROWS_AS(rt::IopModuleRegistry(specs), std::invalid_argument);
    CHECK(rt::is_iop_service("interpret"));
}
