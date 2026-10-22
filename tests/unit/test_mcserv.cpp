// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_stop.h"
#include "runtime/io/memcard/memory_card.h"
#include "runtime/io/memcard/memory_card_store.h"
#include "runtime/machine/cost_model.h"
#include "runtime/machine/rtc.h"
#include "support/sif_client.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rt = dq8::rt;
namespace mc = dq8::rt::mc;
using dq8::test::SifClient;

namespace {

constexpr std::uint32_t mc_client = 0x00012000;
constexpr std::uint32_t send = 0x00030000;
constexpr std::uint32_t recv = 0x00031000;
constexpr std::uint32_t info = 0x00042000;
constexpr std::uint32_t buffer = 0x00050000;
constexpr std::uint32_t table = 0x00060000;
constexpr std::uint32_t record = 0x00061000;

const rt::RtcDateTime epoch{2005, 11, 15, 12, 0, 0};

// An IOP with a card in port 0 (and optionally port 1), the virtual RTC at `epoch`.
struct CardRig {
    explicit CardRig(bool formatted = false, bool second = false)
        : image(dq8::core::iso::Image::from_bytes(dq8::test::test_disc().build(&layout))),
          source(rt::make_iso_disc_source(image)), rtc(epoch),
          card0(rt::make_memory_card_store(formatted, epoch)),
          card1(second ? rt::make_memory_card_store(true, epoch) : nullptr),
          iop(std::make_unique<rt::Iop>([&] {
              rt::IopConfig c;
              c.disc = source.get();
              c.input = &input;
              c.modules = dq8::test::test_modules();
              c.reset_image = "MODULES/IOPRP.IMG";
              c.cards[0] = card0.get();
              c.cards[1] = card1.get();
              c.rtc = &rtc;
              return c;
          }())),
          client(m) {
        m.attach(*iop);
        client.init_rpc();
        const std::uint32_t loader = 0x00012F00;
        REQUIRE(client.bind(loader, 0x80000006u) != 0);
        std::vector<std::uint8_t> req(0x200, 0);
        const std::string path = "cdrom0:\\MODULES\\MCSERV.IRX;1";
        std::memcpy(req.data() + 8, path.data(), path.size());
        client.write(0x00033000, req);
        REQUIRE(client.call(loader, 0, 0x00033000, 0x200, 0x00033200, 8));
        REQUIRE(client.bind(mc_client, 0x80000400u) != 0);
        REQUIRE(client.call(mc_client, 0xFE, send, 0x30, recv, 12));
    }

    // A no-wait call as libmc makes it; returns the reply word once the call ended.
    std::int32_t call(std::uint32_t fno, const std::vector<std::uint8_t>& request) {
        client.write(send, request);
        REQUIRE(
            client.call(mc_client, fno, send, static_cast<std::uint32_t>(request.size()), recv, 4, false));
        REQUIRE(client.wait_for([&] { return client.call_done(mc_client); }));
        return static_cast<std::int32_t>(client.read32(recv));
    }

    static std::vector<std::uint8_t> desc() { return std::vector<std::uint8_t>(0x30, 0); }
    static std::vector<std::uint8_t> named(std::uint32_t port, std::string_view path,
                                           std::uint32_t flags = 0) {
        std::vector<std::uint8_t> r(0x414, 0);
        SifClient::put(r, 0x00, port);
        SifClient::put(r, 0x04, 1); // the game's save code passes slot 1
        SifClient::put(r, 0x08, flags);
        std::memcpy(r.data() + 0x14, path.data(), path.size());
        return r;
    }

    std::int32_t get_info(std::uint32_t port) {
        auto d = desc();
        SifClient::put(d, 0x04, port);
        SifClient::put(d, 0x08, 1);
        SifClient::put(d, 0x0C, 1);
        SifClient::put(d, 0x10, 1);
        SifClient::put(d, 0x14, 1);
        SifClient::put(d, 0x1C, info);
        client.write(info, std::vector<std::uint8_t>(0xC0, 0xFF));
        return call(0x01, d);
    }
    std::int32_t on_port(std::uint32_t fno, std::uint32_t port) {
        auto d = desc();
        SifClient::put(d, 0x04, port);
        SifClient::put(d, 0x08, 1);
        return call(fno, d);
    }
    std::int32_t on_fd(std::uint32_t fno, std::int32_t fd) {
        auto d = desc();
        SifClient::put(d, 0x00, static_cast<std::uint32_t>(fd));
        return call(fno, d);
    }
    // libmc's write: the bytes up to the next 16-byte boundary of the EE buffer travel inline.
    std::int32_t write(std::int32_t fd, const std::vector<std::uint8_t>& data, std::uint32_t ee) {
        client.write(ee, data);
        auto d = desc();
        SifClient::put(d, 0x00, static_cast<std::uint32_t>(fd));
        const std::uint32_t head =
            data.size() <= 16 ? static_cast<std::uint32_t>(data.size()) : (16 - (ee & 15)) & 15;
        SifClient::put(d, 0x14, head);
        SifClient::put(d, 0x0C, static_cast<std::uint32_t>(data.size()) - head);
        SifClient::put(d, 0x18, data.size() <= 16 ? 0 : ee + head);
        std::memcpy(d.data() + 0x20, data.data(), head);
        return call(0x06, d);
    }
    std::int32_t read(std::int32_t fd, std::uint32_t size) {
        auto d = desc();
        SifClient::put(d, 0x00, static_cast<std::uint32_t>(fd));
        SifClient::put(d, 0x0C, size);
        SifClient::put(d, 0x18, buffer);
        SifClient::put(d, 0x1C, info);
        return call(0x05, d);
    }
    std::int32_t get_dir(std::uint32_t port, std::string_view path, std::uint32_t maxent) {
        auto r = named(port, path);
        SifClient::put(r, 0x0C, maxent);
        SifClient::put(r, 0x10, table);
        return call(0x0D, r);
    }
    std::string entry_name(std::uint32_t index) {
        const auto b = client.read(table + index * 0x40 + 0x20, 32);
        return std::string(reinterpret_cast<const char*>(b.data()));
    }

    std::vector<rt::MachineEvent> events(std::string_view type) const {
        std::vector<rt::MachineEvent> out;
        for (const auto& e : m.emitted) {
            if (e.type == type) {
                out.push_back(e);
            }
        }
        return out;
    }

    dq8::test::Layout layout;
    dq8::core::iso::Image image;
    std::unique_ptr<rt::IDiscSource> source;
    dq8::test::TableInput input;
    rt::VirtualRtc rtc;
    std::unique_ptr<rt::IMemoryCardStore> card0;
    std::unique_ptr<rt::IMemoryCardStore> card1;
    dq8::test::FakeMachine m;
    std::unique_ptr<rt::Iop> iop;
    SifClient client;
};

std::vector<std::uint8_t> pattern(std::size_t n, std::uint8_t seed) {
    std::vector<std::uint8_t> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::uint8_t>(seed ^ (i * 13));
    }
    return out;
}

template <typename T> T field(const rt::MachineEvent& e, std::string_view key) {
    for (const auto& [k, v] : e.fields) {
        if (k == key) {
            return std::get<T>(v);
        }
    }
    FAIL("no field " << key);
    return T{};
}

} // namespace

TEST_CASE("mcserv: init reports the versions the library requires", "[iop][mcserv]") {
    CardRig rig;
    REQUIRE(rig.client.call(mc_client, 0xFE, send, 0x30, recv, 12));
    CHECK(rig.client.read32(recv) == 0);
    CHECK(rig.client.read32(recv + 4) >= 0x20A);
    CHECK(rig.client.read32(recv + 8) >= 0x20E);
}

TEST_CASE("mcserv: GetInfo sequences for an unformatted card, a formatted card and no card",
          "[iop][mcserv]") {
    CardRig rig(false, true);
    CHECK(rig.get_info(0) == -2); // new and unformatted
    CHECK(rig.client.read32(info + 0x00) == 2);
    CHECK(rig.client.read32(info + 0x04) == 0);
    CHECK(rig.client.read32(info + 0x90) == 0);
    CHECK(rig.get_info(0) == 0); // the same card
    CHECK(rig.client.read32(info + 0x90) == 0);

    CHECK(rig.get_info(1) == -1); // new and formatted
    CHECK(rig.client.read32(info + 0x04) == 8000);
    CHECK(rig.client.read32(info + 0x90) == 1);
    CHECK(rig.get_info(1) == 0);

    CardRig empty_port_1;
    CHECK(empty_port_1.get_info(1) < -10);
    CHECK(empty_port_1.client.read32(info + 0x00) == 0);
    CHECK(empty_port_1.call(0x02, CardRig::named(1, "/X", 1)) < -10);

    // One mc_info per port and state change; GetInfo itself makes no mc_op.
    CHECK(rig.events("mc_info").size() == 2);
    CHECK(rig.events("mc_op").empty());
}

TEST_CASE("mcserv: format, mkdir, write, read, getdir, rename and delete", "[iop][mcserv]") {
    CardRig rig;
    REQUIRE(rig.get_info(0) == -2);
    REQUIRE(rig.call(0x02, CardRig::named(0, "/BASLUS-21207dq8_1", 0x40)) == -2); // unformatted
    REQUIRE(rig.on_port(0x10, 0) == 0);                                           // format
    REQUIRE(rig.get_info(0) == 0);
    CHECK(rig.client.read32(info + 0x90) == 1);

    REQUIRE(rig.call(0x02, CardRig::named(0, "/BASLUS-21207dq8_1", 0x40)) == 0); // mkdir
    CHECK(rig.call(0x02, CardRig::named(0, "/BASLUS-21207dq8_1", 0x40)) == -4);  // exists
    const std::int32_t fd = rig.call(0x02, CardRig::named(0, "/BASLUS-21207dq8_1/icon.sys", 0x202));
    REQUIRE(fd == 0);
    const auto data = pattern(0x3C4, 0x5A);
    CHECK(rig.write(fd, data, buffer + 4) == 0x3C4);             // unaligned: 12 inline bytes, then the body
    CHECK(rig.write(fd, pattern(10, 1), buffer + 0x1000) == 10); // small: all inline
    REQUIRE(rig.on_fd(0x0A, fd) == 0);                           // flush
    REQUIRE(rig.on_fd(0x03, fd) == 0);                           // close
    CHECK(rig.on_fd(0x03, fd) == -4);

    REQUIRE(rig.get_dir(0, "/BASLUS-21207dq8???", 30) == 1);
    CHECK(rig.entry_name(0) == "BASLUS-21207dq8_1");
    REQUIRE(rig.get_dir(0, "/BASLUS-21207dq8_1/*", 8) == 3);
    CHECK(rig.entry_name(0) == ".");
    CHECK(rig.entry_name(1) == "..");
    CHECK(rig.entry_name(2) == "icon.sys");
    CHECK(rig.client.read32(table + 2 * 0x40 + 0x10) == 0x3C4 + 10);
    // Timestamps come from the virtual RTC: the fake machine's clock is still in the first second.
    const auto created = rig.client.read(table + 2 * 0x40, 8);
    CHECK(created[1] == 0);  // seconds
    CHECK(created[3] == 12); // hour
    CHECK(created[4] == 15); // day
    CHECK((created[6] | created[7] << 8) == 2005);

    const std::int32_t rfd = rig.call(0x02, CardRig::named(0, "/BASLUS-21207dq8_1/icon.sys", 1));
    REQUIRE(rfd >= 0);
    REQUIRE(rig.read(rfd, 0x1000) == 0x3C4 + 10);
    CHECK(rig.client.read(buffer, 0x3C4) == data);
    CHECK(rig.client.read32(info) == 0); // the fix-up block's head and tail counts
    CHECK(rig.client.read32(info + 4) == 0);
    auto seek = CardRig::desc();
    SifClient::put(seek, 0x00, static_cast<std::uint32_t>(rfd));
    SifClient::put(seek, 0x10, 4);
    SifClient::put(seek, 0x14, 0);
    CHECK(rig.call(0x04, seek) == 4);
    REQUIRE(rig.on_fd(0x03, rfd) == 0);

    // Rename: SetInfo with flags 0x10 and the new name in the EE record.
    std::vector<std::uint8_t> rec(0x40, 0);
    std::memcpy(rec.data() + 0x20, "icon.err", 8);
    rig.client.write(record, rec);
    auto rename = CardRig::named(0, "/BASLUS-21207dq8_1/icon.sys", 0x10);
    SifClient::put(rename, 0x10, record);
    CHECK(rig.call(0x0E, rename) == 0);
    REQUIRE(rig.get_dir(0, "/BASLUS-21207dq8_1/icon.err", 1) == 1);

    CHECK(rig.call(0x0F, CardRig::named(0, "/BASLUS-21207dq8_1")) == -6); // not empty
    CHECK(rig.call(0x0F, CardRig::named(0, "/BASLUS-21207dq8_1/icon.err")) == 0);
    CHECK(rig.call(0x0F, CardRig::named(0, "/BASLUS-21207dq8_1")) == 0);
    CHECK(rig.get_dir(0, "/BASLUS-21207dq8???", 30) == 0);
    CHECK(rig.on_port(0x11, 0) == 0); // unformat
    CHECK(rig.get_info(0) == 0);
    CHECK(rig.client.read32(info + 0x90) == 0);

    // Every operation but GetInfo is reported, with its port, path and result.
    const auto ops = rig.events("mc_op");
    REQUIRE(ops.size() >= 10);
    CHECK(field<std::string>(ops[0], "op") == "mkdir");
    CHECK(field<std::string>(ops[0], "path") == "/BASLUS-21207dq8_1");
    CHECK(field<std::int64_t>(ops[0], "result") == -2);
    CHECK(field<std::string>(ops[1], "op") == "format");
    bool renamed = false;
    for (const auto& e : ops) {
        renamed = renamed || field<std::string>(e, "op") == "rename";
    }
    CHECK(renamed);
    CHECK(rig.card0->card().formatted() == false);
}

TEST_CASE("mcserv: replies take the card's time", "[iop][mcserv]") {
    CardRig rig(true);
    REQUIRE(rig.get_info(0) == -1);
    const std::int32_t fd = rig.call(0x02, CardRig::named(0, "/f", 0x202));
    REQUIRE(fd >= 0);
    const auto start = rig.m.clock().now();
    REQUIRE(rig.write(fd, pattern(0x1000, 3), buffer) == 0x1000);
    const rt::CostModel cost;
    CHECK(rig.m.clock().now() - start >= cost.mc_reply(0x1000));
}

TEST_CASE("mcserv: functions and modes the game does not use stop the guest", "[iop][mcserv]") {
    SECTION("an unknown function") {
        CardRig rig(true);
        rig.client.write(send, CardRig::desc());
        rig.client.call(mc_client, 0x12, send, 0x30, recv, 4, false);
        CHECK_THROWS_AS(rig.client.wait_for([] { return false; }), rt::GuestStop);
    }
    SECTION("a getdir continuation") {
        CardRig rig(true);
        auto r = CardRig::named(0, "/*", 1);
        rig.client.write(send, r);
        rig.client.call(mc_client, 0x0D, send, 0x414, recv, 4, false);
        CHECK_THROWS_AS(rig.client.wait_for([] { return false; }), rt::GuestStop);
    }
    SECTION("open flags beyond the known ones") {
        CardRig rig(true);
        rig.client.write(send, CardRig::named(0, "/x", 0x1000));
        rig.client.call(mc_client, 0x02, send, 0x414, recv, 4, false);
        CHECK_THROWS_AS(rig.client.wait_for([] { return false; }), rt::GuestStop);
    }
}
