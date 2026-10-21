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

constexpr std::uint32_t init_client = 0x00012000;
constexpr std::uint32_t scmd_client = 0x00012040;
constexpr std::uint32_t ncmd_client = 0x00012080;
constexpr std::uint32_t search_client = 0x000120C0;
constexpr std::uint32_t send = 0x00030000;
constexpr std::uint32_t recv = 0x00031000;

std::uint32_t scmd(SifClient& c, std::uint32_t fno, std::uint32_t arg, std::uint32_t recv_size = 4) {
    c.write32(send, arg);
    REQUIRE(c.call(scmd_client, fno, send, 4, recv, recv_size));
    return c.read32(recv);
}

// sceCdSearchFile: the name, and the EE address the IOP writes the file record to.
std::uint32_t search(SifClient& c, const std::string& name, std::uint32_t dest) {
    std::vector<std::uint8_t> req(0x12C, 0);
    std::memcpy(req.data() + 0x24, name.data(), name.size());
    SifClient::put(req, 0x124, dest);
    c.write(send, req);
    REQUIRE(c.call(search_client, 0, send, 0x12C, recv, 4));
    return c.read32(recv);
}

} // namespace

TEST_CASE("cdvdfsv: init reports versions the library accepts", "[iop][cdvd]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    REQUIRE(c.bind(init_client, 0x80000592u) != 0);
    c.write32(send, 0);
    REQUIRE(c.call(init_client, 0, send, 4, recv, 0x10));
    CHECK((c.read32(recv + 4) >> 8) >= 2);
    CHECK((c.read32(recv + 8) >> 8) >= 2);
    CHECK(c.read32(recv + 0xC) == 0);
}

TEST_CASE("cdvdfsv: drive queries describe a readable PS2 DVD", "[iop][cdvd]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    REQUIRE(c.bind(scmd_client, 0x80000593u) != 0);
    CHECK(scmd(c, 0x03, 0) == 0x14); // disc type
    CHECK(scmd(c, 0x04, 0) == 0);    // last error
    CHECK(scmd(c, 0x05, 2, 8) == 1); // tray request
    CHECK(c.read32(recv + 4) == 0);  //   the tray did not move
    CHECK(scmd(c, 0x22, 2) == 1);    // media mode
    CHECK(scmd(c, 0x23, 0x22) == 1); // thread priority
    for (const std::uint32_t sid : {0x8000059Au, 0x8000059Cu}) {
        REQUIRE(c.bind(0x00012100, sid) != 0);
        c.write32(send, 0);
        REQUIRE(c.call(0x00012100, 0, send, 4, recv, 4));
        CHECK(c.read32(recv) == 2); // ready
    }
    c.call(scmd_client, 0x01, send, 0, recv, 16, false); // no clock S-command in this protocol
    CHECK_THROWS_AS(c.wait_for([] { return false; }), rt::GuestStop);
}

TEST_CASE("cdvdfsv: SearchFile writes the file record to the EE", "[iop][cdvd]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    REQUIRE(c.bind(search_client, 0x80000597u) != 0);
    c.write(0x00038000, std::vector<std::uint8_t>(0x24, 0xEE));
    CHECK(search(c, "\\DATA.DAT;1", 0x00038000) == 1);
    CHECK(c.read32(0x00038000) == rig.layout.extent_lba.at("/DATA.DAT"));
    CHECK(c.read32(0x00038004) == 10 * 2048 + 100);
    const auto name = c.read(0x00038008, 16);
    CHECK(std::string(reinterpret_cast<const char*>(name.data())) == "DATA.DAT;1");
    CHECK(search(c, "\\BIN\\TITLE.BIN", 0x00038000) == 1);
    CHECK(c.read32(0x00038000) == rig.layout.extent_lba.at("/BIN/TITLE.BIN"));
    CHECK(search(c, "cdrom0:\\bin\\title.bin;1", 0x00038000) == 1);
    CHECK(search(c, "\\NOPE.DAT;1", 0x00038000) == 0);
}

TEST_CASE("cdvdfsv: a sector read lands in the EE buffer when the virtual read ends", "[iop][cdvd]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    REQUIRE(c.bind(ncmd_client, 0x80000595u) != 0);
    REQUIRE(c.bind(scmd_client, 0x80000593u) != 0);
    REQUIRE(c.call(ncmd_client, 0x0E, 0, 0, recv, 4));
    CHECK(c.read32(recv) == 2); // the N-command disc-ready check before every read

    const std::uint32_t lsn = rig.layout.extent_lba.at("/DATA.DAT");
    const std::uint32_t buffer = 0x00060000;
    const std::uint32_t fixup = 0x00037000;
    c.write(fixup, std::vector<std::uint8_t>(0x90, 0xFF));
    std::vector<std::uint8_t> req(0x18, 0);
    SifClient::put(req, 0x00, lsn + 1);
    SifClient::put(req, 0x04, 3);
    SifClient::put(req, 0x08, buffer);
    req[0x0D] = 1; // spindle control
    SifClient::put(req, 0x10, fixup);
    SifClient::put(req, 0x14, 0x00037100);
    c.write(send, req);
    const rt::Cycles sent = rig.m.clock().now();
    REQUIRE(c.call(ncmd_client, 1, send, 0x18, 0, 0, false));
    const rt::Cycles due = sent + rig.m.cost().cdvd_read(3 * 2048);
    c.run_until(due - 1);
    CHECK_FALSE(c.call_done(ncmd_client));
    CHECK(c.read(buffer, 16) == std::vector<std::uint8_t>(16, 0)); // nothing before the end packet
    REQUIRE(c.wait_for([&] { return c.call_done(ncmd_client); }));
    CHECK(c.ends(ncmd_client) == 1);
    const auto data = dq8::test::seeded_bytes(4, 10 * 2048 + 100);
    CHECK(c.read(buffer, 3 * 2048) ==
          std::vector<std::uint8_t>(data.begin() + 2048, data.begin() + 4 * 2048));
    CHECK(c.read32(fixup) == 0);     // no unaligned head to copy
    CHECK(c.read32(fixup + 4) == 0); // no unaligned tail to copy
    CHECK(scmd(c, 0x04, 0) == 0);

    c.write32(send, lsn);
    REQUIRE(c.call(ncmd_client, 5, send, 4, 0, 0)); // seek
}

TEST_CASE("cdvdfsv: a read outside the disc stops", "[iop][cdvd]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    REQUIRE(c.bind(ncmd_client, 0x80000595u) != 0);
    std::vector<std::uint8_t> req(0x18, 0);
    SifClient::put(req, 0x00, rig.source->sector_count() - 1);
    SifClient::put(req, 0x04, 2);
    SifClient::put(req, 0x08, 0x00060000);
    c.write(send, req);
    c.call(ncmd_client, 1, send, 0x18, 0, 0, false);
    CHECK_THROWS_AS(c.wait_for([] { return false; }), rt::GuestStop);
}
