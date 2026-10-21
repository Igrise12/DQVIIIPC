// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_stop.h"
#include "runtime/io/pad_state.h"
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

constexpr std::uint32_t pad_client = 0x00012000;
constexpr std::uint32_t block = 0x00030000; // the library sends and receives through one 128-byte block
constexpr std::uint32_t area = 0x00041000;  // two 128-byte frames, 64-byte aligned

void load_padman(SifClient& c) {
    const std::uint32_t loader = 0x00012F00;
    REQUIRE(c.bind(loader, 0x80000006u) != 0);
    std::vector<std::uint8_t> req(0x200, 0);
    const std::string path = "cdrom0:\\MODULES\\PADMAN.IRX;1";
    std::memcpy(req.data() + 8, path.data(), path.size());
    c.write(0x00031000, req);
    REQUIRE(c.call(loader, 0, 0x00031000, 0x200, 0x00031200, 8));
    REQUIRE(c.bind(pad_client, 0x80000100u) != 0);
}

std::vector<std::uint8_t> command(SifClient& c, std::uint32_t cmd, std::uint32_t a = 0, std::uint32_t b = 0,
                                  std::uint32_t d = 0) {
    std::vector<std::uint8_t> req(0x80, 0);
    SifClient::put(req, 0x00, cmd);
    SifClient::put(req, 0x04, a);
    SifClient::put(req, 0x08, b);
    SifClient::put(req, 0x10, d);
    c.write(block, req);
    REQUIRE(c.call(pad_client, 1, block, 0x80, block, 0x80));
    return c.read(block, 0x80);
}

// What the EE library does at port open: both frames say "not ready yet".
void init_area(SifClient& c) {
    for (std::uint32_t f = 0; f < 2; ++f) {
        std::vector<std::uint8_t> frame(0x80, 0);
        std::fill(frame.begin(), frame.begin() + 0x20, 0xFF);
        frame[0x70] = 5;
        frame[0x71] = 2;
        c.write(area + f * 0x80, frame);
    }
}

// scePadRead's choice: the frame with the larger counter.
std::vector<std::uint8_t> current_frame(SifClient& c) {
    const auto c0 = static_cast<std::int32_t>(c.read32(area + 0x58));
    const auto c1 = static_cast<std::int32_t>(c.read32(area + 0xD8));
    return c.read(c0 < c1 ? area + 0x80 : area, 0x80);
}

std::uint16_t game_buttons(const std::vector<std::uint8_t>& f) {
    return static_cast<std::uint16_t>(((f[2] << 8) | f[3]) ^ 0xFFFF);
}

} // namespace

TEST_CASE("padman: version, init and port open as the pad library asks", "[iop][padman]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    load_padman(c);
    CHECK((SifClient::get(command(c, 0x12), 0x0C) >> 8) == 4);
    CHECK(SifClient::get(command(c, 0x10), 0x0C) == 1);
    init_area(c);
    const auto open = command(c, 0x01, 0, 0, area);
    CHECK(SifClient::get(open, 0x0C) == 1);
    const std::uint32_t actuators = SifClient::get(open, 0x14);
    CHECK(actuators != 0);
    // SetActDirect has no command: the EE DMAs its 32-byte blocks to the actuator buffer.
    const std::uint32_t id = c.dma_to_iop(0x00031000, actuators + 0x20, 0x20);
    REQUIRE(c.wait_for([&] { return c.dma_stat(id) < 0; }));
    CHECK(SifClient::get(command(c, 0x06, 0, 0, 3), 0x14) == 1);
    CHECK(SifClient::get(command(c, 0x08, 0, 0), 0x14) == 1);
    CHECK(SifClient::get(command(c, 0x01, 2, 0, area), 0x0C) == 0); // no third port
    // 0x80000101 binds but the library never calls it.
    CHECK(c.bind(0x00012040, 0x80000101u) != 0);
    c.write32(block, 0x0B);
    c.call(pad_client, 1, block, 0x80, block, 0x80, false);
    CHECK_THROWS_AS(c.wait_for([] { return false; }), rt::GuestStop);
}

TEST_CASE("padman: every vblank writes the next frame from the input source", "[iop][padman]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    load_padman(c);
    init_area(c);
    REQUIRE(SifClient::get(command(c, 0x01, 0, 0, area), 0x0C) == 1);

    rt::PadState pressed;
    pressed.buttons = rt::pad_start | rt::pad_cross;
    pressed.lx = 0x10;
    rig.input.states[{0, 0}] = pressed;
    rig.m.fire_vblank_start();
    auto f = current_frame(c);
    CHECK(c.read32(area + 0xD8) == 1);
    CHECK(f[0x70] == 6); // stable
    CHECK(f[0x71] == 0); // request complete
    CHECK(f[0x72] == 1);
    CHECK(f[0x65] == 0x73);
    CHECK(f[0x00] == 0x00);
    CHECK(f[0x01] == 0x73);
    CHECK(game_buttons(f) == 0x0840); // start and cross in the library's bit order
    CHECK(f[0x06] == 0x10);
    CHECK(f[0x07] == 0x80);
    CHECK(SifClient::get(f, 0x60) >= 8);
    CHECK(SifClient::get(f, 0x60) <= 32);
    CHECK(f[0x50] == 0x04);
    CHECK(f[0x52] == 0x07);
    CHECK(f[0x69] == 1);

    rig.m.fire_vblank_start(); // field 1: nothing pressed
    f = current_frame(c);
    CHECK(c.read32(area + 0x58) == 2);
    CHECK(game_buttons(f) == 0);

    rig.input.fallback.connected = false;
    rig.m.fire_vblank_start();
    f = current_frame(c);
    CHECK(c.read32(area + 0xD8) == 3);
    CHECK(f[0x70] == 0); // no pad
    CHECK(rig.input.samples == 3);
}

TEST_CASE("padman: nothing is sampled before a port is open", "[iop][padman]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    load_padman(c);
    rig.m.fire_vblank_start();
    rig.m.fire_vblank_end();
    CHECK(rig.input.samples == 0);
}
