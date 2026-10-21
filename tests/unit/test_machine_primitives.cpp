// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/machine/clock.h"
#include "runtime/machine/cost_model.h"
#include "runtime/machine/events.h"
#include "runtime/machine/interrupts.h"
#include "runtime/machine/mmio.h"
#include "runtime/machine/spin_detector.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dq8::rt;

TEST_CASE("clock: charge, deadline and fast-forward", "[machine][clock]") {
    VirtualClock c;
    CHECK(c.now() == 0);
    c.set_deadline(100);
    c.charge(60);
    CHECK_FALSE(c.due());
    c.charge(40);
    CHECK(c.due());
    c.advance_to(50); // never backwards
    CHECK(c.now() == 100);
    c.advance_to(500);
    CHECK(c.now() == 500);
    c.set_deadline(1000);
    CHECK_FALSE(c.due());
    c.force_safe_point();
    CHECK(c.due());
    c.reset();
    CHECK(c.now() == 0);
}

TEST_CASE("clock: unit conversions are exact", "[machine][clock]") {
    CHECK(cycles_from_us(1'000'000) == ee_clock_hz);
    CHECK(cycles_from_us(1) == 294);
    CHECK(cycles_from_us(20) == 5898);
    CHECK(cycles_for_bytes(0, 1000) == 0);
    CHECK(cycles_for_bytes(1, ee_clock_hz) == 1);
    CHECK(cycles_for_bytes(40'000'000, 40'000'000) == ee_clock_hz);
    CHECK(bus_clock_hz * 2 == ee_clock_hz);
}

TEST_CASE("video timing: NTSC fields and lines from exact rationals", "[machine][clock]") {
    const VideoTiming ntsc = VideoTiming::ntsc();
    CHECK(ntsc.field_start(0) == 0);
    // 294,912,000 * 1001 / 60000 = 4,920,115.2 cycles per field.
    CHECK(ntsc.field_start(1) == 4'920'115);
    CHECK(ntsc.field_start(5) == 24'600'576);
    // 60000 fields at 60000/1001 Hz take exactly 1001 seconds.
    CHECK(ntsc.field_start(60'000) == 1001 * ee_clock_hz);
    // 525 lines per two fields: 1050 lines are exactly two frames.
    CHECK(ntsc.line_start(1050) == ntsc.field_start(4));
    CHECK(ntsc.line_start(1) == 18'743);
    CHECK(ntsc.vblank_end(0) > ntsc.field_start(0));
    CHECK(ntsc.vblank_end(0) < ntsc.field_start(1));
    CHECK(ntsc.vblank_end(3) - ntsc.field_start(3) == ntsc.vblank_end(0));

    CHECK(ntsc.fields_started(0) == 1);
    CHECK(ntsc.fields_started(4'920'114) == 1);
    CHECK(ntsc.fields_started(4'920'115) == 2);
    CHECK(ntsc.fields_started(ntsc.field_start(1000)) == 1001);
    CHECK(ntsc.fields_started(ntsc.field_start(1000) - 1) == 1000);

    const VideoTiming pal = VideoTiming::pal();
    CHECK(pal.field_start(50) == ee_clock_hz);
    CHECK(pal.line_start(625) == pal.field_start(2));
}

TEST_CASE("events: run in time order, ties in scheduling order", "[machine][events]") {
    EventQueue q;
    std::vector<std::string> ran;
    q.schedule(30, "c", [&](Cycles when) { ran.push_back("c@" + std::to_string(when)); });
    q.schedule(10, "a", [&](Cycles) { ran.push_back("a"); });
    q.schedule(10, "b", [&](Cycles) { ran.push_back("b"); });
    const EventId d = q.schedule(20, "d", [&](Cycles) { ran.push_back("d"); });
    CHECK(q.size() == 4);
    CHECK(q.next_time() == 10);
    CHECK(q.cancel(d));
    CHECK_FALSE(q.cancel(d));
    CHECK(q.run_due(9) == 0);
    CHECK(q.run_due(25) == 2);
    CHECK(ran == std::vector<std::string>{"a", "b"});
    const auto pending = q.pending();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].tag == "c");
    CHECK(q.run_due(100) == 1);
    CHECK(ran.back() == "c@30");
    CHECK(q.empty());
    CHECK_FALSE(q.next_time().has_value());
}

TEST_CASE("events: a callback may schedule more work due in the same pass", "[machine][events]") {
    EventQueue q;
    std::vector<int> ran;
    q.schedule(5, "first", [&](Cycles when) {
        ran.push_back(1);
        q.schedule(when, "same-time", [&](Cycles) { ran.push_back(2); });
        q.schedule(when + 100, "later", [&](Cycles) { ran.push_back(3); });
    });
    CHECK(q.run_due(10) == 2);
    CHECK(ran == std::vector<int>{1, 2});
    CHECK(q.next_time() == 105);
}

TEST_CASE("interrupts: INTC write semantics and pending state", "[machine][interrupts]") {
    InterruptController ic;
    int hooks = 0;
    ic.set_raise_hook([&] { ++hooks; });
    ic.raise_intc(IntcLine::vblank_start);
    CHECK(ic.intc_stat() == 0x4u);
    CHECK_FALSE(ic.int0_pending()); // masked
    CHECK(hooks == 0);
    ic.write_intc_mask(0x4u); // toggle on
    CHECK(ic.int0_pending());
    CHECK(hooks == 1);
    ic.write_intc_mask(0x4u); // toggle off
    CHECK_FALSE(ic.int0_pending());
    ic.write_intc_mask(0x804u);
    ic.raise_intc(IntcLine::timer2);
    CHECK(ic.intc_pending() == 0x804u);
    ic.write_intc_stat(0x4u); // write 1 to clear
    CHECK(ic.intc_stat() == 0x800u);
    ic.write_intc_stat(0x0u);
    CHECK(ic.intc_stat() == 0x800u);
    ic.reset();
    CHECK(ic.intc_stat() == 0);
    CHECK(ic.intc_mask() == 0);
}

TEST_CASE("interrupts: D_STAT clears status and toggles masks", "[machine][interrupts]") {
    InterruptController ic;
    ic.raise_dmac(DmaChannel::sif0);
    CHECK(ic.dmac_stat() == (1u << 5));
    CHECK_FALSE(ic.int1_pending());
    ic.write_dmac_stat(1u << (16 + 5)); // toggle CIM5 on
    CHECK(ic.int1_pending());
    CHECK(ic.dmac_pending_channels() == (1u << 5));
    ic.write_dmac_stat(1u << 5); // clear CIS5
    CHECK_FALSE(ic.int1_pending());
    CHECK(ic.dmac_stat() == (1u << (16 + 5)));
    ic.raise_dmac_status(InterruptController::dstat_beis);
    CHECK(ic.int1_pending()); // bus errors are never masked
    ic.write_dmac_stat(InterruptController::dstat_beis);
    CHECK_FALSE(ic.pending());
    CHECK(to_string(DmaChannel::gif) == "gif");
    CHECK(to_string(IntcLine::vblank_end) == "vblank-end");
}

namespace {

struct RecordingDevice final : IMmioDevice {
    std::string label;
    std::vector<std::uint32_t> reads;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;
    bool decline = false;
    explicit RecordingDevice(std::string l) : label(std::move(l)) {}
    std::string_view name() const override { return label; }
    bool read(std::uint32_t offset, std::uint32_t width, std::uint8_t* out, const MmioContext&) override {
        if (decline) {
            return false;
        }
        reads.push_back(offset);
        std::memset(out, 0, width);
        out[0] = static_cast<std::uint8_t>(offset);
        return true;
    }
    bool write(std::uint32_t offset, std::uint32_t width, const std::uint8_t* in,
               const MmioContext&) override {
        std::uint32_t v = 0;
        std::memcpy(&v, in, width < 4 ? width : 4);
        writes.emplace_back(offset, v);
        return !decline;
    }
};

} // namespace

TEST_CASE("mmio: ranges route by physical address", "[machine][mmio]") {
    MmioBus bus;
    RecordingDevice timers("timers"), gs("gs"), iop("iop-ram");
    bus.map(0x10000000, 0x40, timers);
    bus.map(0x12000000, 0x2000, gs);
    bus.map(0x1C000000, 2u << 20, iop);
    std::uint8_t b[16] = {};
    CHECK(bus.read(0x10000010, 4, b, {}));
    CHECK(b[0] == 0x10);
    CHECK(bus.read(0x12001000, 8, b, {}));
    CHECK(gs.reads.back() == 0x1000);
    CHECK(bus.read(0x1C1FFFF0, 16, b, {})); // last quadword of a 2 MiB range spanning 512 pages
    CHECK(iop.reads.back() == 0x1FFFF0);
    CHECK_FALSE(bus.read(0x10000040, 4, b, {})); // just past the timers
    CHECK_FALSE(bus.read(0x1000003C, 8, b, {})); // would cross the range end
    CHECK_FALSE(bus.read(0x00100000, 4, b, {})); // outside the device space
    const std::uint32_t v = 0xDEADBEEF;
    CHECK(bus.write(0x10000020, 4, reinterpret_cast<const std::uint8_t*>(&v), {}));
    CHECK(timers.writes.back() == std::make_pair(0x20u, 0xDEADBEEFu));
    gs.decline = true;
    CHECK_FALSE(bus.read(0x12000000, 8, b, {}));
    const auto hit = bus.find(0x12000070);
    REQUIRE(hit);
    CHECK(hit->offset == 0x70);
    CHECK(bus.mappings().size() == 3);
    CHECK(bus.mappings().front().base == 0x10000000);
}

TEST_CASE("mmio: bad and overlapping ranges are refused", "[machine][mmio]") {
    MmioBus bus;
    RecordingDevice a("a"), b("b");
    bus.map(0x10003000, 0x100, a);
    CHECK_THROWS_AS(bus.map(0x10003080, 0x100, b), std::invalid_argument);
    CHECK_THROWS_AS(bus.map(0x10004001, 0x10, b), std::invalid_argument);
    CHECK_THROWS_AS(bus.map(0x0FFFFFF0, 0x20, b), std::invalid_argument);
    CHECK_THROWS_AS(bus.map(0x10005000, 0, b), std::invalid_argument);
    bus.map(0x10003100, 0x100, b); // adjacent on the same page is fine
    std::uint8_t out[4];
    CHECK(bus.read(0x10003100, 4, out, {}));
    CHECK(b.reads.size() == 1);
    CHECK(a.reads.empty());
}

TEST_CASE("spin detector: N identical reads in a row, reset by anything else", "[machine][spin]") {
    SpinDetector s(4);
    CHECK_FALSE(s.note(0x100, 0x12001000, 0));
    CHECK_FALSE(s.note(0x100, 0x12001000, 0));
    CHECK_FALSE(s.note(0x100, 0x12001000, 0));
    CHECK(s.note(0x100, 0x12001000, 0));
    CHECK_FALSE(s.note(0x100, 0x12001000, 0));      // starts over
    CHECK_FALSE(s.note(0x100, 0x12001000, 0x2000)); // the value changed
    CHECK_FALSE(s.note(0x100, 0x12001000, 0x2000));
    s.reset();
    CHECK_FALSE(s.note(0x100, 0x12001000, 0x2000));
    CHECK_FALSE(s.note(0x104, 0x12001000, 0x2000)); // another pc
}

TEST_CASE("cost model: m3-v1 completions are never instantaneous", "[machine][cost]") {
    const CostModel c;
    CHECK(c.name == "m3-v1");
    CHECK(c.rpc_reply(0) == cycles_from_us(20));
    CHECK(c.rpc_reply(40'000) > c.rpc_reply(0));
    CHECK(c.cdvd_read(2048) > cycles_from_us(100));
    CHECK(c.spin_reads == 64);
}
