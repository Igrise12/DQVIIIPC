// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_stop.h"
#include "runtime/iop/iop_memory.h"
#include "runtime/iop/iop_modules.h"
#include "support/sif_client.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

namespace rt = dq8::rt;
using dq8::test::IopRig;
using dq8::test::SifClient;

namespace {

std::size_t count_events(const dq8::test::FakeMachine& m, std::string_view type) {
    std::size_t n = 0;
    for (const auto& e : m.emitted) {
        if (e.type == type) {
            ++n;
        }
    }
    return n;
}

// Loads a module through the loadfile server.
void load(SifClient& c, std::string_view path, std::string_view args = {}) {
    const std::uint32_t loader = 0x00012F00;
    if (c.read32(loader + 0x24) == 0) {
        REQUIRE(c.bind(loader, 0x80000006u) != 0);
        REQUIRE(c.call(loader, 0xFF, 0, 0, 0x00031000, 4));
    }
    std::vector<std::uint8_t> req(0x200, 0);
    SifClient::put(req, 0, static_cast<std::uint32_t>(args.size()));
    std::memcpy(req.data() + 8, path.data(), path.size());
    std::memcpy(req.data() + 0x104, args.data(), args.size());
    c.write(0x00030000, req);
    REQUIRE(c.call(loader, 0, 0x00030000, 0x200, 0x00031000, 8));
}

} // namespace

TEST_CASE("iop rpc: an unregistered server id binds as server 0 and is no event", "[iop][rpc]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    CHECK(c.bind(0x00012000, 0x80000701u) == 0);
    CHECK(c.bind(0x00012000, 0x80000701u) == 0);
    CHECK(c.read32(0x00012000 + 0x14) == 0);
    CHECK(count_events(rig.m, "rpc_bind") == 0);

    // Once the module is loaded the same id binds, and each successful bind is an event with the handle.
    load(c, "cdrom0:\\MODULES\\SDRDRV.IRX;1");
    const auto before = count_events(rig.m, "rpc_bind");
    const std::uint32_t server = c.bind(0x00012000, 0x80000701u);
    CHECK(server != 0);
    CHECK(c.read32(0x00012000 + 0x14) != 0);
    CHECK(c.read32(0x00012000 + 0x14) != server);
    REQUIRE(count_events(rig.m, "rpc_bind") == before + 1);
    const auto& e = rig.m.emitted.back();
    REQUIRE(e.fields.size() == 2);
    CHECK(e.fields[0].first == "sid");
    CHECK(std::get<std::uint64_t>(e.fields[0].second) == 0x80000701u);
    CHECK(e.fields[1].first == "server");
    CHECK(std::get<std::uint64_t>(e.fields[1].second) == server);
}

TEST_CASE("iop rpc: calls are events only while rpc tracing is on", "[iop][rpc]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    const std::uint32_t client = 0x00012000;
    REQUIRE(c.bind(client, 0x80000003u) != 0);
    c.write32(0x00030000, 0x40);
    REQUIRE(c.call(client, 1, 0x00030000, 4, 0x00032000, 4));
    CHECK(count_events(rig.m, "rpc_call") == 0);
    CHECK(count_events(rig.m, "trace") == 0);

    rig.m.trace_mask = rt::trace_bit(rt::TraceCategory::rpc) | rt::trace_bit(rt::TraceCategory::sif);
    REQUIRE(c.call(client, 1, 0x00030000, 4, 0x00032000, 4));
    REQUIRE(count_events(rig.m, "rpc_call") == 1);
    const rt::MachineEvent* call = nullptr;
    for (const auto& e : rig.m.emitted) {
        if (e.type == "rpc_call") {
            call = &e;
        }
    }
    REQUIRE(call != nullptr);
    CHECK(std::get<std::uint64_t>(call->fields[0].second) == 0x80000003u);
    CHECK(std::get<std::uint64_t>(call->fields[1].second) == 1);
    CHECK(count_events(rig.m, "trace") >= 2); // the transfer and the packet landing at least
}

TEST_CASE("iop rpc: the standard servers bind with distinct handles and buffers", "[iop][rpc]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    std::set<std::uint32_t> words;
    std::uint32_t client = 0x00012000;
    for (const std::uint32_t sid : {0x80000001u, 0x80000003u, 0x80000006u, 0x80000592u, 0x80000593u,
                                    0x80000595u, 0x80000597u, 0x8000059Au, 0x8000059Cu}) {
        const std::uint32_t server = c.bind(client, sid);
        CHECK(server != 0);
        CHECK(words.insert(server).second);
        CHECK(words.insert(c.read32(client + 0x14)).second);
        client += 0x40;
    }
    CHECK(count_events(rig.m, "rpc_bind") == 9);
}

TEST_CASE("iop rpc: a reply is not delivered before its virtual ready time", "[iop][rpc]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    load(c, "cdrom0:\\MODULES\\PCMPLAY.IRX;1");
    const std::uint32_t client = 0x00012000;
    REQUIRE(c.bind(client, 0x123456u) != 0);
    c.write(0x00032000, std::vector<std::uint8_t>(64, 0xEE));
    c.write(0x00030000, dq8::test::seeded_bytes(5, 32));
    const rt::Cycles sent = rig.m.clock().now();
    REQUIRE(c.call(client, 7, 0x00030000, 32, 0x00032000, 64, false));
    const rt::Cycles earliest = sent + rig.m.cost().rpc_reply(64);
    c.run_until(earliest - 1);
    CHECK_FALSE(c.call_done(client));
    REQUIRE(c.wait_for([&] { return c.call_done(client); }));
    CHECK(rig.m.clock().now() >= earliest);
    CHECK(c.ends(client) == 1);
    CHECK(c.read(0x00032000, 64) == std::vector<std::uint8_t>(64, 0)); // a null server answers zeros

    REQUIRE(count_events(rig.m, "rpc_null") == 1);
    REQUIRE(c.call(client, 7, 0x00030000, 32, 0x00032000, 64));
    CHECK(count_events(rig.m, "rpc_null") == 1); // only the first call of each function number is reported
}

TEST_CASE("iop rpc: a call without an end packet frees its slot through the data", "[iop][rpc]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    const std::uint32_t client = 0x00012000;
    REQUIRE(c.bind(client, 0x80000003u) != 0);
    c.write32(0x00030000, 0x100);
    const auto interrupts = c.interrupts_serviced();
    REQUIRE(c.call(client, 1, 0x00030000, 4, 0x00032000, 4, true, false));
    CHECK(c.read32(0x00032000) != 0); // the allocated IOP address arrived
    CHECK(c.ends(client) == 0);
    CHECK(c.interrupts_serviced() == interrupts); // no END, no interrupt
}

TEST_CASE("iop rpc: one server answers one call at a time", "[iop][rpc]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    load(c, "cdrom0:\\MODULES\\SDRDRV.IRX;1");
    REQUIRE(c.bind(0x00012000, 0x80000701u) != 0);
    c.write32(0x00012040 + 0x24, c.read32(0x00012000 + 0x24));
    c.write32(0x00012040 + 0x14, c.read32(0x00012000 + 0x14));
    const rt::Cycles sent = rig.m.clock().now();
    REQUIRE(c.call(0x00012000, 1, 0x00030000, 16, 0x00032000, 4000, false));
    REQUIRE(c.call(0x00012040, 1, 0x00030000, 16, 0x00034000, 4000, false));
    REQUIRE(c.wait_for([&] { return c.call_done(0x00012000) && c.call_done(0x00012040); }));
    CHECK(rig.m.clock().now() >= sent + 2 * rig.m.cost().rpc_reply(4000));
}

TEST_CASE("iop rpc: a call to an unknown server handle stops", "[iop][rpc]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    c.write32(0x00012000 + 0x24, 0x00123450);
    c.call(0x00012000, 0, 0, 0, 0, 0, false);
    CHECK_THROWS_AS(c.wait_for([] { return false; }), rt::GuestStop);
}

TEST_CASE("iop rpc: an unknown system command stops", "[iop][rpc]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    c.send_command(0x80000017u, std::vector<std::uint8_t>(16, 0), 0, 0, 0);
    CHECK_THROWS_AS(c.wait_for([] { return false; }), rt::GuestStop);
}

TEST_CASE("iop rpc: rdata copies IOP RAM to the EE and ends the request", "[iop][rpc]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    const auto data = dq8::test::seeded_bytes(11, 48);
    c.write(0x00030000, data);
    const std::uint32_t id = c.dma_to_iop(0x00030000, 0x00050000, 48);
    REQUIRE(c.wait_for([&] { return c.dma_stat(id) < 0; }));
    const std::uint32_t receiver = 0x00012000;
    c.write32(receiver, 0x20011000);
    std::vector<std::uint8_t> p(0x40, 0);
    SifClient::put(p, 0x1C, receiver);
    SifClient::put(p, 0x20, 0x00050000);
    SifClient::put(p, 0x24, 0x00033000);
    SifClient::put(p, 0x28, 48);
    c.send_command(0x8000000Cu, p, 0, 0, 0, 0, 0x20011000);
    REQUIRE(c.wait_for([&] { return c.read32(receiver) == 0; }));
    CHECK(c.read(0x00033000, 48) == data);
}

TEST_CASE("iop modules: the catalogue and the module table", "[iop][modules]") {
    CHECK(rt::is_iop_service("fileio"));
    CHECK(rt::is_iop_service("padman.v1"));
    CHECK(rt::is_iop_service("null:0x80000701"));
    CHECK(rt::is_iop_service("null:1193046"));
    CHECK_FALSE(rt::is_iop_service("null:"));
    CHECK_FALSE(rt::is_iop_service("null:0"));
    CHECK_FALSE(rt::is_iop_service("null:0x1x"));
    CHECK_FALSE(rt::is_iop_service("libsd"));
    CHECK(rt::IopModuleRegistry::module_name("cdrom0:\\MODULES\\PADMAN.IRX;1") == "PADMAN");
    CHECK(rt::IopModuleRegistry::module_name("rom0:sio2man") == "SIO2MAN");
    CHECK(rt::IopModuleRegistry::module_name("cdrom0:/modules/mcserv.irx") == "MCSERV");
    CHECK_THROWS_AS(rt::IopModuleRegistry({{"PADMAN", {"padman.v2"}}}), std::invalid_argument);
    CHECK_THROWS_AS(rt::IopModuleRegistry({{"PADMAN", {}}, {"padman", {}}}), std::invalid_argument);
    CHECK_THROWS_AS(rt::IopModuleRegistry({{"", {}}}), std::invalid_argument);
    rt::IopModuleRegistry reg({{"padman", {"padman.v1"}}});
    REQUIRE(reg.find("PADMAN") != nullptr);
    CHECK(reg.find("PADMAN")->name == "PADMAN");
}

namespace {
rt::IopConfig iop_config(const rt::IDiscSource* disc, rt::IInputSource* input,
                         std::vector<rt::IopModuleSpec> modules) {
    rt::IopConfig c;
    c.disc = disc;
    c.input = input;
    c.modules = std::move(modules);
    return c;
}
} // namespace

TEST_CASE("iop: configuration errors are refused at construction", "[iop]") {
    dq8::test::TableInput input;
    CHECK_THROWS_AS(rt::Iop(iop_config(nullptr, &input, {})), std::invalid_argument);
    const auto image = dq8::core::iso::Image::from_bytes(dq8::test::test_disc().build());
    const auto disc = rt::make_iso_disc_source(image);
    CHECK_THROWS_AS(rt::Iop(iop_config(disc.get(), nullptr, {{"PADMAN", {"padman.v1"}}})),
                    std::invalid_argument);
    CHECK_THROWS_AS(rt::Iop(iop_config(disc.get(), &input, {{"X", {"nope"}}})), std::invalid_argument);
}

TEST_CASE("iop memory: first-fit, 16-byte blocks, deterministic reuse", "[iop][memory]") {
    rt::IopMemory mem;
    const auto a = mem.allocate(10);
    const auto b = mem.allocate(32);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(*a == rt::IopMemory::heap_base);
    CHECK(*b == rt::IopMemory::heap_base + 16);
    mem.free(*a);
    CHECK(mem.allocate(16) == a);
    CHECK(mem.allocate(17) == *b + 32);
    CHECK_FALSE(mem.allocate(0));
    CHECK_FALSE(mem.allocate(rt::IopMemory::heap_end));
    mem.write32(0x1FFFFC, 0x11223344);
    CHECK(mem.read32(0x1FFFFC) == 0x11223344);
    CHECK_THROWS_AS(mem.read32(0x1FFFFE), std::out_of_range);
    mem.reset();
    CHECK(mem.read32(0x1FFFFC) == 0);
    CHECK(mem.allocate(16) == rt::IopMemory::heap_base);
}
