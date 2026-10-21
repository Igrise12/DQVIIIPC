// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_stop.h"
#include "support/sif_client.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rt = dq8::rt;
using dq8::test::IopRig;
using dq8::test::SifClient;

namespace {

constexpr std::uint32_t mc_client = 0x00012000;
constexpr std::uint32_t send = 0x00030000;
constexpr std::uint32_t recv = 0x00031000;
constexpr std::uint32_t info = 0x00042000;

void load_mcserv(SifClient& c) {
    const std::uint32_t loader = 0x00012F00;
    REQUIRE(c.bind(loader, 0x80000006u) != 0);
    std::vector<std::uint8_t> req(0x200, 0);
    const std::string path = "cdrom0:\\MODULES\\MCSERV.IRX;1";
    std::memcpy(req.data() + 8, path.data(), path.size());
    c.write(0x00033000, req);
    REQUIRE(c.call(loader, 0, 0x00033000, 0x200, 0x00033200, 8));
    REQUIRE(c.bind(mc_client, 0x80000400u) != 0);
}

std::int32_t get_info(SifClient& c, std::uint32_t port) {
    std::vector<std::uint8_t> desc(0x30, 0);
    SifClient::put(desc, 0x04, port);
    SifClient::put(desc, 0x08, 1); // the game's save code passes slot 1
    SifClient::put(desc, 0x0C, 1);
    SifClient::put(desc, 0x10, 1);
    SifClient::put(desc, 0x14, 1);
    SifClient::put(desc, 0x1C, info);
    c.write(send, desc);
    c.write(info, std::vector<std::uint8_t>(0xC0, 0xFF));
    REQUIRE(c.call(mc_client, 0x01, send, 0x30, recv, 4, false));
    REQUIRE(c.wait_for([&] { return c.call_done(mc_client); }));
    return static_cast<std::int32_t>(c.read32(recv));
}

} // namespace

TEST_CASE("mcserv: init reports the versions the library requires", "[iop][mcserv]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    load_mcserv(c);
    REQUIRE(c.call(mc_client, 0xFE, send, 0x30, recv, 12));
    CHECK(c.read32(recv) == 0);
    CHECK(c.read32(recv + 4) >= 0x20A);
    CHECK(c.read32(recv + 8) >= 0x20E);
}

TEST_CASE("mcserv: port 0 holds a present, unformatted card; port 1 is empty", "[iop][mcserv]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    load_mcserv(c);
    CHECK(get_info(c, 0) == -2);
    CHECK(c.read32(info + 0x00) == 2); // a PS2 card
    CHECK(c.read32(info + 0x04) == 0); // no free clusters
    CHECK(c.read32(info + 0x90) == 0); // unformatted
    CHECK(get_info(c, 0) == -2);
    CHECK(get_info(c, 1) < -10); // no card
    CHECK(c.read32(info + 0x00) == 0);

    std::vector<std::uint8_t> named(0x414, 0);
    const std::string path = "/BASLUS-21207";
    std::memcpy(named.data() + 0x14, path.data(), path.size());
    c.write(send, named);
    REQUIRE(c.call(mc_client, 0x02, send, 0x414, recv, 4)); // open on the unformatted card
    CHECK(static_cast<std::int32_t>(c.read32(recv)) == -2);

    std::size_t reported = 0;
    for (const auto& e : rig.m.emitted) {
        if (e.type == "mc_info") {
            ++reported;
        }
    }
    CHECK(reported == 2);

    c.call(mc_client, 0x10, send, 0x30, recv, 4, false); // format
    CHECK_THROWS_AS(c.wait_for([] { return false; }), rt::GuestStop);
}
