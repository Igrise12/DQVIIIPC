// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_stop.h"
#include "runtime/iop/sif_link.h"
#include "runtime/machine/mmio.h"
#include "support/sif_client.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rt = dq8::rt;
using dq8::test::IopRig;
using dq8::test::SifClient;

namespace {

constexpr std::uint32_t smflg_boot = 0x00070000;

std::uint32_t mmio_read32(rt::MmioBus& bus, std::uint32_t phys) {
    std::uint8_t b[4] = {};
    REQUIRE(bus.read(phys, 4, b, {}));
    std::uint32_t v = 0;
    std::memcpy(&v, b, 4);
    return v;
}

void mmio_write32(rt::MmioBus& bus, std::uint32_t phys, std::uint32_t v) {
    std::uint8_t b[4];
    std::memcpy(b, &v, 4);
    REQUIRE(bus.write(phys, 4, b, {}));
}

} // namespace

TEST_CASE("sif: the IOP has booted at power-on", "[sif]") {
    IopRig rig;
    SifClient& c = rig.client;
    CHECK(c.get_reg(4) == smflg_boot);
    CHECK(c.get_reg(2) == rt::SifLink::command_buffer);
    CHECK(c.get_reg(1) == 0);
    CHECK(c.get_reg(3) == 0);
    for (std::uint32_t i = 0; i < 32; ++i) {
        CHECK(c.get_reg(0x80000000u | i) == 0);
    }
}

TEST_CASE("sif: register writes follow the hardware's set and clear rules", "[sif]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.set_reg(3, 0x5);
    c.set_reg(3, 0x2);
    CHECK(c.get_reg(3) == 0x7); // MSFLG: EE writes set bits
    c.set_reg(4, 0x10000);
    CHECK(c.get_reg(4) == (smflg_boot & ~0x10000u)); // SMFLG: EE writes clear bits
    c.set_reg(2, 0x1234);
    CHECK(c.get_reg(2) == rt::SifLink::command_buffer); // SMCOM belongs to the IOP
    c.set_reg(1, 0xABCD);
    CHECK(c.get_reg(1) == 0xABCD);
    c.set_reg(0x80000005u, 77);
    CHECK(c.get_reg(0x80000005u) == 77);
    CHECK_THROWS_AS(c.get_reg(9), rt::GuestStop);
    CHECK_THROWS_AS(c.set_reg(0x80000020u, 1), rt::GuestStop);
}

TEST_CASE("sif: the register block and IOP RAM are mapped for EE loads and stores", "[sif]") {
    IopRig rig;
    rt::MmioBus& bus = rig.m.mmio();
    CHECK(mmio_read32(bus, 0x1000F230) == smflg_boot);
    CHECK(mmio_read32(bus, 0x1000F210) == rt::SifLink::command_buffer);
    mmio_write32(bus, 0x1000F220, 0x100);
    CHECK(mmio_read32(bus, 0x1000F220) == 0x100);
    mmio_write32(bus, 0x1000F230, 0x40000);
    CHECK(rig.client.get_reg(4) == 0x30000);
    const auto polls = rig.m.polls;
    mmio_read32(bus, 0x1000F230);
    CHECK(rig.m.polls == polls + 1);

    // IOP RAM at 0x1C000000 shows what the IOP holds, and EE stores reach it.
    mmio_write32(bus, 0x1C000000 + 0x40000, 0xCAFEF00D);
    CHECK(mmio_read32(bus, 0x1C000000 + 0x40000) == 0xCAFEF00D);
}

TEST_CASE("sif: an EE transfer lands in IOP RAM at a later virtual time", "[sif]") {
    IopRig rig;
    SifClient& c = rig.client;
    const auto data = dq8::test::seeded_bytes(9, 0x100);
    c.write(0x00030000, data);
    const rt::Cycles t0 = rig.m.clock().now();
    const std::uint32_t id = c.dma_to_iop(0x20030000, 0x00080000, 0x100); // uncached source alias
    REQUIRE(id != 0);
    CHECK(c.dma_stat(id) >= 0);
    std::uint8_t probe[4] = {};
    rt::MmioBus& bus = rig.m.mmio();
    REQUIRE(bus.read(0x1C080000, 4, probe, {}));
    CHECK(std::all_of(probe, probe + 4, [](std::uint8_t b) { return b == 0; }));

    REQUIRE(c.wait_for([&] { return c.dma_stat(id) < 0; }));
    CHECK(rig.m.clock().now() >= t0 + 16 * rig.m.cost().dma_per_qword);
    std::vector<std::uint8_t> landed(0x100);
    for (std::uint32_t i = 0; i < 0x100; i += 4) {
        REQUIRE(bus.read(0x1C080000 + i, 4, landed.data() + i, {}));
    }
    CHECK(landed == data);
    // IOP addresses lose their segment bits and mirror every 2 MiB.
    const std::uint32_t id2 = c.dma_to_iop(0x00030000, 0xA0280010, 4);
    REQUIRE(c.wait_for([&] { return c.dma_stat(id2) < 0; }));
    REQUIRE(bus.read(0x1C080010, 4, probe, {}));
    CHECK(std::equal(probe, probe + 4, data.begin()));
}

TEST_CASE("sif: transfers outside IOP RAM or EE RAM stop the guest", "[sif]") {
    IopRig rig;
    SifClient& c = rig.client;
    CHECK_THROWS_AS(c.dma_to_iop(0x00030000, 0x00900000, 16), rt::GuestStop);
    CHECK_THROWS_AS(c.dma_to_iop(0x00030000, 0x001FFFF0, 32), rt::GuestStop);
    CHECK_THROWS_AS(c.dma_to_iop(0x12000000, 0x00001000, 16), rt::GuestStop);
    CHECK_THROWS_AS(c.kcall("sceSifSetDma", {SifClient::descriptors, 0}), rt::GuestStop);
}

TEST_CASE("sif: the command-layer and RPC-layer handshakes", "[sif]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    CHECK(c.iop_command_buffer() == rt::SifLink::command_buffer);
    CHECK(c.get_reg(0x80000000u) == rt::SifLink::command_buffer);
    CHECK(c.get_reg(0x80000001u) == SifClient::command_data);
    CHECK(c.get_reg(0x80000002u) == 1);
    CHECK(c.ee_sregs()[0] != 0); // set by the IOP in answer to init option 1
    REQUIRE(c.received().size() == 1);
    CHECK(SifClient::get(c.received()[0], 0x8) == 0x80000001u);

    // A second command-layer init finds the IOP known and only moves its receive buffer.
    c.init_cmd();
    REQUIRE(c.wait_for([&] { return rig.m.events().empty(); }));
    CHECK(c.received().size() == 1);
}

TEST_CASE("sif: an IOP packet waits until the EE arms SIF0, one packet per arming", "[sif]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    const std::uint32_t client_a = 0x00012000;
    const std::uint32_t client_b = 0x00012040;

    // Disarm, then queue two binds: nothing reaches the EE.
    c.kcall("sceSifStopDma", {});
    const auto before = c.received().size();
    c.write32(client_a, 0x20011000);
    c.write32(client_b, 0x20011040);
    std::vector<std::uint8_t> p(0x40, 0);
    SifClient::put(p, 0x1C, client_a);
    SifClient::put(p, 0x20, 0x80000001u);
    c.send_command(0x80000009u, p, 0, 0, 0, 0, 0x20011000);
    SifClient::put(p, 0x1C, client_b);
    c.send_command(0x80000009u, p, 0, 0, 0, 0, 0x20011040);
    c.run_until(rig.m.clock().now() + 10'000'000);
    CHECK(c.received().size() == before);
    CHECK((rig.m.interrupts().dmac_stat() & (1u << 5)) == 0);

    // With the channel masked the client's handler stays away: arming once delivers exactly one packet.
    rig.m.interrupts().write_dmac_stat(1u << (16 + 5));
    c.kcall("sceSifSetDChain", {});
    c.run_until(rig.m.clock().now() + 10'000'000);
    CHECK((rig.m.interrupts().dmac_stat() & (1u << 5)) != 0);
    CHECK(c.read32(SifClient::receive_slot + 0x1C) == client_a);
    CHECK(c.received().size() == before);

    // Unmasked, the handler takes the first packet and re-arms, and the second follows.
    rig.m.interrupts().write_dmac_stat(1u << (16 + 5));
    c.run_until(rig.m.clock().now() + 10'000'000);
    CHECK(c.received().size() == before + 2);
    CHECK(c.read32(client_a + 0x24) != 0);
    CHECK(c.read32(client_b + 0x24) != 0);
}

TEST_CASE("sif: the IOP reset handshake keeps the standard services and drops modules", "[sif][iop]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    const std::uint32_t loader = 0x00012000;
    REQUIRE(c.bind(loader, 0x80000006u) != 0);
    // Load PADMAN so its server exists before the reset.
    std::vector<std::uint8_t> req(0x200, 0);
    const std::string path = "cdrom0:\\MODULES\\PADMAN.IRX;1";
    std::memcpy(req.data() + 8, path.data(), path.size());
    c.write(0x00030000, req);
    REQUIRE(c.call(loader, 0, 0x00030000, 0x200, 0x00031000, 8));
    REQUIRE(c.bind(0x00012040, 0x80000100u) != 0);

    const auto events = rig.m.emitted.size();
    REQUIRE(c.reset_iop("rom0:UDNL cdrom0:\\MODULES\\IOPRP.IMG;1") != 0);
    CHECK((c.get_reg(4) & smflg_boot) == 0);
    REQUIRE(c.sync_iop());
    CHECK(c.get_reg(4) == smflg_boot);
    CHECK(c.get_reg(2) == rt::SifLink::command_buffer);
    REQUIRE(rig.m.emitted.size() > events);
    CHECK(rig.m.emitted[events].type == "iop_reset");

    c.init_rpc();                                // the full init path again, as after the game's reboot
    CHECK(c.bind(0x00012080, 0x80000001u) != 0); // standard services remain
    CHECK(c.bind(0x000120C0, 0x80000592u) != 0);
    CHECK(c.bind(0x00012100, 0x80000100u) == 0); // PADMAN's server went with the reboot
}

TEST_CASE("sif: a reset with an image the profile does not name stops", "[sif][iop]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    c.reset_iop("rom0:UDNL cdrom0:\\MODULES\\PADMAN.IRX;1");
    CHECK_THROWS_AS(c.sync_iop(), rt::GuestStop);
}

TEST_CASE("sif: a reset with an image missing from the disc stops", "[sif][iop]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    c.reset_iop("rom0:UDNL cdrom0:\\MODULES\\NOPE.IMG;1");
    CHECK_THROWS_AS(c.sync_iop(), rt::GuestStop);
}
