// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "host/m4_wiring.h"
#include "runtime/gs/gs_backends.h"
#include "runtime/gs/gs_pipeline.h"
#include "runtime/io/pad_feedback.h"
#include "runtime/io/pad_state.h"
#include "runtime/platform/platform.h"
#include "support/sif_client.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rt = dq8::rt;
using dq8::test::SifClient;

namespace {

constexpr std::uint32_t pad_client = 0x00012000;
constexpr std::uint32_t block = 0x00030000;
constexpr std::uint32_t area = 0x00041000;
constexpr std::uint32_t act_block = 0x00032000;

struct Recorder final : rt::IPadFeedbackSink {
    void actuators(unsigned port, std::uint64_t vblank, const rt::PadActuators& state) override {
        calls.push_back({port, vblank, state});
    }
    struct Call {
        unsigned port;
        std::uint64_t vblank;
        rt::PadActuators state;
    };
    std::vector<Call> calls;
};

// An IOP whose pad service reports to a recording feedback sink.
struct PadRig {
    PadRig()
        : image(dq8::core::iso::Image::from_bytes(dq8::test::test_disc().build(&layout))),
          source(rt::make_iso_disc_source(image)), iop(std::make_unique<rt::Iop>([&] {
              rt::IopConfig c;
              c.disc = source.get();
              c.input = &input;
              c.modules = dq8::test::test_modules();
              c.reset_image = "MODULES/IOPRP.IMG";
              c.pad_feedback = &recorder;
              return c;
          }())),
          client(m) {
        m.attach(*iop);
        client.init_rpc();
        const std::uint32_t loader = 0x00012F00;
        REQUIRE(client.bind(loader, 0x80000006u) != 0);
        std::vector<std::uint8_t> req(0x200, 0);
        const std::string path = "cdrom0:\\MODULES\\PADMAN.IRX;1";
        std::memcpy(req.data() + 8, path.data(), path.size());
        client.write(0x00031000, req);
        REQUIRE(client.call(loader, 0, 0x00031000, 0x200, 0x00031200, 8));
        REQUIRE(client.bind(pad_client, 0x80000100u) != 0);
    }

    std::vector<std::uint8_t> command(std::uint32_t cmd, std::uint32_t port = 0, std::uint32_t slot = 0,
                                      std::uint32_t at_0c = 0, std::uint32_t at_10 = 0,
                                      const std::vector<std::uint8_t>& bytes_0c = {}) {
        std::vector<std::uint8_t> req(0x80, 0);
        SifClient::put(req, 0x00, cmd);
        SifClient::put(req, 0x04, port);
        SifClient::put(req, 0x08, slot);
        SifClient::put(req, 0x0C, at_0c);
        SifClient::put(req, 0x10, at_10);
        std::memcpy(req.data() + 0x0C, bytes_0c.data(), bytes_0c.size());
        client.write(block, req);
        REQUIRE(client.call(pad_client, 1, block, 0x80, block, 0x80));
        return client.read(block, 0x80);
    }

    // Opens port 0 and returns the IOP actuator buffer.
    std::uint32_t open() {
        const auto reply = command(0x01, 0, 0, 0, area);
        REQUIRE(SifClient::get(reply, 0x0C) == 1);
        return SifClient::get(reply, 0x14);
    }

    // SetActDirect as the library does it: a 32-byte block DMAed into the half the new counter picks.
    void act_direct(std::uint32_t actuators, std::int32_t counter, std::uint8_t b0, std::uint8_t b1) {
        std::vector<std::uint8_t> blk(0x20, 0);
        SifClient::put(blk, 0x00, static_cast<std::uint32_t>(counter));
        SifClient::put(blk, 0x04, 1);
        SifClient::put(blk, 0x08, 6);
        blk[0x0C] = b0;
        blk[0x0D] = b1;
        client.write(act_block, blk);
        const std::uint32_t id =
            client.dma_to_iop(act_block, actuators + 0x20 * (static_cast<std::uint32_t>(counter) & 1u), 0x20);
        REQUIRE(client.wait_for([&] { return client.dma_stat(id) < 0; }));
    }

    std::vector<std::uint8_t> frame() {
        const auto c0 = static_cast<std::int32_t>(client.read32(area + 0x58));
        const auto c1 = static_cast<std::int32_t>(client.read32(area + 0xD8));
        return client.read(c0 < c1 ? area + 0x80 : area, 0x80);
    }

    std::vector<rt::MachineEvent> rumble_events() const {
        std::vector<rt::MachineEvent> out;
        for (const auto& e : m.emitted) {
            if (e.type == "pad_rumble") {
                out.push_back(e);
            }
        }
        return out;
    }

    dq8::test::Layout layout;
    dq8::core::iso::Image image;
    std::unique_ptr<rt::IDiscSource> source;
    dq8::test::TableInput input;
    Recorder recorder;
    dq8::test::FakeMachine m;
    std::unique_ptr<rt::Iop> iop;
    SifClient client;
};

template <typename T> T field(const rt::MachineEvent& e, std::string_view key) {
    for (const auto& [k, v] : e.fields) {
        if (k == key) {
            return std::get<T>(v);
        }
    }
    FAIL("no field " << key);
    return T{};
}

struct FakePlatform final : rt::IPlatform {
    bool pump_events() override { return true; }
    rt::PadState pad(unsigned) const override { return {}; }
    void present(const dq8::gs::FrameImage&, double) override {}
    void set_rumble(unsigned port, std::uint16_t low, std::uint16_t high) override {
        calls.push_back({port, low, high});
    }
    struct Call {
        unsigned port;
        std::uint16_t low;
        std::uint16_t high;
    };
    std::vector<Call> calls;
};

} // namespace

TEST_CASE("padman: every command word the game's libpad can send", "[iop][padman]") {
    PadRig rig;
    CHECK((SifClient::get(rig.command(0x12), 0x0C) >> 8) == 4);
    CHECK(SifClient::get(rig.command(0x10), 0x0C) == 1);
    rig.open();
    CHECK(SifClient::get(rig.command(0x0C), 0x0C) == 2);    // GetPortMax
    CHECK(SifClient::get(rig.command(0x0D, 0), 0x0C) == 1); // GetSlotMax
    CHECK(SifClient::get(rig.command(0x0D, 5), 0x0C) == 0);
    CHECK(SifClient::get(rig.command(0x0B), 0x1C) == 1);          // SetVrefParam
    CHECK(SifClient::get(rig.command(0x08), 0x14) == 1);          // SetActAlign
    CHECK(SifClient::get(rig.command(0x06, 0, 0, 1), 0x14) == 1); // SetMainMode analog
    rig.command(0x18, 0, 0);                                      // the callback switch
    rig.command(0x18, 1, 0);
    for (int f = 0; f < 3; ++f) {
        rig.m.fire_vblank_start();
    }
    CHECK(rig.frame()[0x01] == 0x73);
    CHECK(SifClient::get(rig.command(0x0F), 0x0C) == 1); // End: the ports close
    const auto samples = rig.input.samples;
    rig.m.fire_vblank_start();
    CHECK(rig.input.samples == samples);
}

TEST_CASE("padman: pressure bytes only once the program asks for them", "[iop][padman]") {
    PadRig rig;
    rig.open();
    rt::PadState pad;
    pad.pressure[6] = 0xC0; // cross
    rig.input.fallback = pad;
    REQUIRE(SifClient::get(rig.command(0x06, 0, 0, 1), 0x14) == 1);
    for (int f = 0; f < 3; ++f) {
        rig.m.fire_vblank_start();
    }
    auto f = rig.frame();
    CHECK(f[0x01] == 0x73);
    CHECK(f[0x08 + 6] == 0);
    CHECK(SifClient::get(rig.command(0x0A, 0, 0, 0xFFF), 0x10) == 1); // SetButtonInfo: every button
    rig.m.fire_vblank_start();
    f = rig.frame();
    CHECK(f[0x01] == 0x79);
    CHECK(f[0x65] == 0x79);
    CHECK(f[0x08 + 6] == 0xC0);
    CHECK(SifClient::get(rig.command(0x0A, 0, 0, 0), 0x10) == 1);
    rig.m.fire_vblank_start();
    CHECK(rig.frame()[0x01] == 0x73);
}

TEST_CASE("padman: SetActDirect blocks drive the motors through the alignment", "[iop][padman]") {
    PadRig rig;
    const std::uint32_t actuators = rig.open();
    REQUIRE(actuators != 0);
    rig.m.fire_vblank_start();
    CHECK(rig.rumble_events().empty()); // nothing sent: the motors stay off without a report

    // Without an alignment the direct bytes drive nothing.
    rig.act_direct(actuators, 1, 1, 0x80);
    rig.m.fire_vblank_start();
    CHECK(rig.rumble_events().empty());

    // The game's table: byte 0 the small motor, byte 1 the large one.
    REQUIRE(SifClient::get(rig.command(0x08, 0, 0, 0, 0, {0, 1, 0xFF, 0xFF, 0xFF, 0xFF}), 0x14) == 1);
    rig.m.fire_vblank_start();
    auto events = rig.rumble_events();
    REQUIRE(events.size() == 1);
    CHECK(field<std::uint64_t>(events[0], "port") == 0);
    CHECK(field<bool>(events[0], "small"));
    CHECK(field<std::uint64_t>(events[0], "large") == 0x80);
    REQUIRE(rig.recorder.calls.size() == 1);
    CHECK(rig.recorder.calls[0].state == rt::PadActuators{true, 0x80});

    rig.m.fire_vblank_start(); // unchanged: no report
    CHECK(rig.rumble_events().size() == 1);

    rig.act_direct(actuators, 2, 0, 0x20); // the newer block, in the other half
    rig.m.fire_vblank_start();
    events = rig.rumble_events();
    REQUIRE(events.size() == 2);
    CHECK_FALSE(field<bool>(events[1], "small"));
    CHECK(field<std::uint64_t>(events[1], "large") == 0x20);

    // A pad that goes away stops its motors.
    rig.input.fallback.connected = false;
    rig.m.fire_vblank_start();
    REQUIRE(rig.recorder.calls.size() == 3);
    CHECK(rig.recorder.calls[2].state == rt::PadActuators{});
}

TEST_CASE("pad feedback: the latest motor state reaches the platform on the main thread", "[pad]") {
    rt::LivePadFeedback feedback;
    FakePlatform platform;
    feedback.apply(platform);
    CHECK(platform.calls.empty());
    feedback.actuators(0, 10, rt::PadActuators{true, 0x10});
    feedback.actuators(0, 11, rt::PadActuators{false, 0xFF});
    feedback.actuators(1, 11, rt::PadActuators{true, 0});
    feedback.actuators(7, 11, rt::PadActuators{true, 0}); // no such port
    feedback.apply(platform);
    REQUIRE(platform.calls.size() == 2);
    CHECK(platform.calls[0].port == 0);
    CHECK(platform.calls[0].low == 0xFFFF);
    CHECK(platform.calls[0].high == 0);
    CHECK(platform.calls[1].port == 1);
    CHECK(platform.calls[1].low == 0);
    CHECK(platform.calls[1].high == 0xFFFF);
    feedback.apply(platform);
    CHECK(platform.calls.size() == 2);
    CHECK(rt::LivePadFeedback::low_frequency(rt::PadActuators{false, 0x80}) == 0x8080);
}

TEST_CASE("wire_feedback: rumble reaches the window's platform, unless headless or --rumble off", "[pad]") {
    dq8::gs::GsPipelineConfig config;
    config.backend = dq8::gs::make_null_gs_backend();
    config.inline_mode = true;
    dq8::gs::GsPipeline pipeline(std::move(config));
    FakePlatform platform;
    const auto wire = [&](bool rumble, rt::IPlatform* with) {
        dq8::host::Options o;
        o.rumble = rumble;
        rt::IopConfig iop;
        auto w = std::make_unique<dq8::host::M4Wiring>(
            dq8::host::M4Wiring{o, nullptr, pipeline, iop, with, {}, {}, {}, {}, {}, {}, {}});
        dq8::host::wire_feedback(*w);
        return std::make_pair(std::move(w), iop.pad_feedback);
    };
    {
        auto [w, sink] = wire(true, nullptr); // headless
        CHECK(sink == nullptr);
        CHECK(w->on_frame.empty());
    }
    {
        auto [w, sink] = wire(false, &platform);
        CHECK(sink == nullptr);
    }
    auto [w, sink] = wire(true, &platform);
    REQUIRE(sink != nullptr);
    REQUIRE(w->on_frame.size() == 1);
    sink->actuators(0, 5, rt::PadActuators{true, 0x40});
    w->on_frame[0]();
    REQUIRE(platform.calls.size() == 1);
    CHECK(platform.calls[0].low == 0x4040);
    CHECK(platform.calls[0].high == 0xFFFF);
    for (const auto& stop : w->on_stop) {
        stop();
    }
    REQUIRE(platform.calls.size() == 3); // both ports' motors off at the end
    CHECK(platform.calls[1].low == 0);
    CHECK(platform.calls[2].port == 1);
}
