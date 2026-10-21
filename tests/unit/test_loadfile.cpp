// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_stop.h"
#include "support/sif_client.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

namespace rt = dq8::rt;
using dq8::test::IopRig;
using dq8::test::SifClient;

namespace {

constexpr std::uint32_t loader = 0x00012000;
constexpr std::uint32_t send = 0x00030000;
constexpr std::uint32_t recv = 0x00031000;

// sceSifLoadModule: argument length, the path at +8 and the arguments at +0x104; replies the module id and
// the module's start result.
std::int32_t load_module(SifClient& c, const std::string& path, const std::string& args = {}) {
    std::vector<std::uint8_t> req(0x200, 0);
    SifClient::put(req, 0x00, static_cast<std::uint32_t>(args.size()));
    std::memcpy(req.data() + 0x08, path.data(), path.size());
    std::memcpy(req.data() + 0x104, args.data(), args.size());
    c.write(send, req);
    c.write32(recv + 4, 0xFFFFFFFFu);
    REQUIRE(c.call(loader, 0, send, 0x200, recv, 8));
    CHECK(c.read32(recv + 4) == 0); // stays resident
    return static_cast<std::int32_t>(c.read32(recv));
}

const rt::MachineEvent* last_event(const dq8::test::FakeMachine& m, std::string_view type) {
    for (auto it = m.emitted.rbegin(); it != m.emitted.rend(); ++it) {
        if (it->type == type) {
            return &*it;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("loadfile: the version query and a module whose services become bindable", "[iop][loadfile]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    REQUIRE(c.bind(loader, 0x80000006u) != 0);
    REQUIRE(c.call(loader, 0xFF, 0, 0, recv, 4));
    CHECK(c.read32(recv) == 0x30303033u);

    CHECK(c.bind(0x00012040, 0x80000100u) == 0);
    CHECK(c.bind(0x00012080, 0x80000400u) == 0);
    const std::int32_t sio2 = load_module(c, "cdrom0:\\MODULES\\SIO2MAN.IRX;1");
    const std::int32_t pad = load_module(c, "cdrom0:\\MODULES\\PADMAN.IRX;1");
    CHECK(sio2 >= 0);
    CHECK(pad > sio2);
    CHECK(c.bind(0x00012040, 0x80000100u) != 0);
    CHECK(c.bind(0x000120C0, 0x80000101u) != 0);
    CHECK(c.bind(0x00012080, 0x80000400u) == 0); // MCSERV is not loaded yet
    CHECK(load_module(c, "cdrom0:\\MODULES\\MCSERV.IRX;1") > pad);
    CHECK(c.bind(0x00012080, 0x80000400u) != 0);

    const std::string args("maxtrack=16\0", 12);
    CHECK(load_module(c, "cdrom0:\\MODULES\\PCMPLAY.IRX;1", args) >= 0);
    const rt::MachineEvent* e = last_event(rig.m, "iop_module");
    REQUIRE(e != nullptr);
    CHECK(std::get<std::string>(e->fields[0].second) == "PCMPLAY");
    CHECK(std::get<std::string>(e->fields[2].second) == "null:0x123456");
    CHECK(std::get<std::string>(e->fields[3].second) == "maxtrack=16");

    // Loading a module again answers the same id and registers nothing twice.
    CHECK(load_module(c, "cdrom0:\\MODULES\\PADMAN.IRX;1") == pad);
}

TEST_CASE("loadfile: a module the profile does not list stops", "[iop][loadfile]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    REQUIRE(c.bind(loader, 0x80000006u) != 0);
    std::vector<std::uint8_t> req(0x200, 0);
    const std::string path = "cdrom0:\\MODULES\\EXTRA.IRX;1";
    std::memcpy(req.data() + 8, path.data(), path.size());
    c.write(send, req);
    c.call(loader, 0, send, 0x200, recv, 8, false);
    CHECK_THROWS_AS(c.wait_for([] { return false; }), rt::GuestStop);
}

TEST_CASE("loadfile: a module missing from the disc stops", "[iop][loadfile]") {
    IopRig rig(dq8::test::test_disc(), {{"LIBSD", {}}});
    SifClient& c = rig.client;
    c.init_rpc();
    REQUIRE(c.bind(loader, 0x80000006u) != 0);
    std::vector<std::uint8_t> req(0x200, 0);
    const std::string path = "cdrom0:\\MODULES\\LIBSD.IRX;1";
    std::memcpy(req.data() + 8, path.data(), path.size());
    c.write(send, req);
    c.call(loader, 0, send, 0x200, recv, 8, false);
    CHECK_THROWS_AS(c.wait_for([] { return false; }), rt::GuestStop);
}

TEST_CASE("loadfile: a module from the IOP's ROM loads by name", "[iop][loadfile]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    REQUIRE(c.bind(loader, 0x80000006u) != 0);
    CHECK(load_module(c, "rom0:SIO2MAN") >= 0);
}
