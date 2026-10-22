// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_stop.h"
#include "runtime/iop/hw/iop_hw.h"
#include "runtime/iop/iop_exec.h"
#include "runtime/machine/events.h"
#include "support/irx_builder.h"
#include "support/sif_client.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace rt = dq8::rt;
namespace m = dq8::test::mips;
namespace ia = dq8::test::iopasm;
using Catch::Matchers::ContainsSubstring;
using dq8::test::IopRig;
using dq8::test::IrxBuilder;
using Label = IrxBuilder::Label;

namespace {

// A 16-bit device at 0x1F900000 serving DMA channels 4 and 7: registers below 0x100 are plain storage, 0x40
// reads 1 once its ready event ran, transfers finish 4,096 cycles after they start (or when the test says).
class FakeDevice final : public rt::IIopBusDevice, public rt::IIopDmaDevice, public rt::IIopHardwareClient {
  public:
    struct Access {
        std::uint32_t offset = 0;
        std::uint32_t width = 0;
        std::uint32_t value = 0;
    };

    std::string_view name() const override { return "FAKEDEV"; }
    bool read(std::uint32_t offset, std::uint32_t width, std::uint32_t& value,
              const rt::IopAccess&) override {
        reads.push_back({offset, width, 0});
        if (offset == 0x40) {
            value = ready ? 1 : 0;
            return true;
        }
        if (offset >= 0x100) {
            return false;
        }
        value = regs[offset];
        return true;
    }
    bool write(std::uint32_t offset, std::uint32_t width, std::uint32_t value,
               const rt::IopAccess&) override {
        writes.push_back({offset, width, value});
        if (offset >= 0x100) {
            return false;
        }
        regs[offset] = value;
        return true;
    }
    rt::IopDmaStart dma_start(const rt::IopDmaRequest& r) override {
        requests.push_back(r);
        return rt::IopDmaStart{deferred, r.at + 4096};
    }
    void dma_stop(std::uint32_t channel, rt::Cycles) override { stops.push_back(channel); }
    void connect(rt::IIopHardwareHost& h) override { host = &h; }

    void ready_at(rt::Cycles t) {
        host->machine().events().schedule(t, "fake_ready", [this](rt::Cycles) { ready = true; });
    }

    rt::IopDeviceMapping mapping() {
        return rt::IopDeviceMapping{0x1F900000u, 0x800, this, this, this, {4, 7}};
    }

    std::map<std::uint32_t, std::uint32_t> regs;
    std::vector<Access> reads;
    std::vector<Access> writes;
    std::vector<rt::IopDmaRequest> requests;
    std::vector<std::uint32_t> stops;
    rt::IIopHardwareHost* host = nullptr;
    bool ready = false;
    bool deferred = false;
};

Label thread_param(IrxBuilder& b, Label entry, std::uint32_t priority) {
    const Label p = b.words({0, 0, 0, 0x800, priority});
    b.pointer(p, 8, entry);
    return p;
}

// A start that spawns `body` (thbase 4 and 6 imported) and returns 0.
void start_spawning(IrxBuilder& b, Label body) {
    const Label p = thread_param(b, body, 20);
    const Label start = b.here();
    b.enter(32);
    b.la(m::a0, p);
    b.jal(b.stub("thbase", 4));
    b.emit(m::move(m::a0, m::v0));
    b.li(m::a1, 0);
    b.jal(b.stub("thbase", 6));
    b.li(m::v0, 0);
    b.leave(32);
    b.entry(start);
}

std::string path_of(const std::string& name) {
    return "cdrom0:\\MODULES\\" + name + ".IRX;1";
}

struct HwRig {
    explicit HwRig(const IrxBuilder& b)
        : rig(dq8::test::disc_with({{"HWTEST", b.build()}}), dq8::test::interpreted({"HWTEST"}),
              "MODULES/IOPRP.IMG", [this](rt::IopConfig& c) { c.devices.push_back(dev.mapping()); }) {
        rig.client.init_rpc();
    }
    void load() { REQUIRE(dq8::test::loadfile_module(rig.client, path_of("HWTEST")).done); }
    void run_us(std::uint64_t us) { rig.client.run_until(rig.m.clock().now() + rt::cycles_from_us(us)); }
    std::uint32_t base() { return rig.iop->interpreter().modules().at(0).base; }
    std::vector<std::uint32_t> log(const IrxBuilder& b, Label l) {
        std::vector<std::uint32_t> out;
        const std::uint32_t at = base() + b.offset_of(l);
        const std::uint32_t n = dq8::test::iop_ram32(*rig.iop, at);
        for (std::uint32_t i = 0; i < n && i < 32; ++i) {
            out.push_back(dq8::test::iop_ram32(*rig.iop, at + 4 + 4 * i));
        }
        return out;
    }
    // Loads the module and expects a stop; returns its message.
    std::string load_until_stop() {
        try {
            (void)dq8::test::loadfile_module(rig.client, path_of("HWTEST"));
            run_us(1000);
        } catch (const rt::GuestStop& s) {
            reason = s.info().reason;
            return s.info().message;
        }
        return "<no stop>";
    }

    FakeDevice dev;
    IopRig rig;
    rt::StopReason reason = rt::StopReason::program_end;
};

// lui $t0, 0x1F80: the IOP's register page in $t0.
void reg_page(IrxBuilder& b) {
    b.emit(m::lui(m::t0, 0x1F80));
}

// The DMA module: a handler for DMA channel 4, a 16-bit and a 32-bit device access, then one transfer.
struct DmaModule {
    IrxBuilder b{"HWTEST"};
    Label log;
    Label seen;
    DmaModule() {
        log = b.zeros(64);
        seen = b.zeros(4);
        const Label sema_param = b.words({0, 0, 0, 1});
        const Label sema = b.zeros(4);
        b.imports("thbase", {4, 6});
        b.imports("thsemap", {4, 7, 8});
        b.imports("intrman", {4, 6});
        const Label body = b.label();
        const Label handler = b.label();
        start_spawning(b, body);

        b.bind(body);
        b.enter(32);
        b.la(m::a0, sema_param);
        b.jal(b.stub("thsemap", 4));
        b.store(sema, 0, m::v0);
        b.li(m::a0, 0x24);
        b.li(m::a1, 1);
        b.la(m::a2, handler);
        b.li(m::a3, 0x77);
        b.jal(b.stub("intrman", 4));
        b.append(log, m::v0);
        b.call(b.stub("intrman", 6), {0x24});
        b.append(log, m::v0);
        b.emit(m::lui(m::t0, 0x1F90));
        b.li(m::t1, 0x1234);
        b.emit(ia::sh(m::t1, 0x10, m::t0));
        b.emit(m::lw(m::t2, 0x20, m::t0));
        b.emit(m::nop());
        b.append(log, m::t2);
        reg_page(b);
        b.emit(m::lw(m::t1, 0x10F0, m::t0)); // DPCR: enable channel 4
        b.emit(m::nop());
        b.emit(m::lui(m::t2, 0x0008));
        b.emit(m::or_(m::t1, m::t1, m::t2));
        b.emit(m::sw(m::t1, 0x10F0, m::t0));
        b.li(m::t1, 0x00100000);
        b.emit(m::sw(m::t1, 0x10C0, m::t0)); // MADR
        b.li(m::t1, 0x00100010);
        b.emit(m::sw(m::t1, 0x10C4, m::t0)); // BCR
        b.li(m::t1, 0x01000201);
        b.emit(m::sw(m::t1, 0x10C8, m::t0)); // CHCR: start, from RAM
        b.load(m::a0, sema);
        b.jal(b.stub("thsemap", 8));
        b.append(log, m::v0);
        reg_page(b);
        b.emit(m::lw(m::t1, 0x10C8, m::t0));
        b.emit(m::nop());
        b.append(log, m::t1);
        b.leave(32);

        b.bind(handler);
        b.enter(32);
        b.store(seen, 0, m::a0);
        b.load(m::a0, sema);
        b.jal(b.stub("thsemap", 7));
        b.li(m::v0, 1);
        b.leave(32);
    }
};

} // namespace

TEST_CASE("iop hw: a DMA transfer on a mapped channel completes into its interrupt handler", "[iop][hw]") {
    DmaModule mod;
    HwRig h(mod.b);
    h.dev.regs[0x20] = 0xAABB;
    h.dev.regs[0x22] = 0xCCDD;
    h.load();
    h.run_us(1000);
    CHECK(h.log(mod.b, mod.log) == std::vector<std::uint32_t>{0, 0, 0xCCDDAABBu, 0, 0x00000201u});
    CHECK(dq8::test::iop_ram32(*h.rig.iop, h.base() + mod.b.offset_of(mod.seen)) == 0x77);
    REQUIRE(h.dev.requests.size() == 1);
    const rt::IopDmaRequest& r = h.dev.requests[0];
    CHECK(r.channel == 4);
    CHECK(r.madr == 0x00100000u);
    CHECK(r.bcr == 0x00100010u);
    CHECK(r.chcr == 0x01000201u);
    // The 16-bit store arrived as is; the 32-bit load as two 16-bit reads, low half first.
    REQUIRE(h.dev.writes.size() == 1);
    CHECK(h.dev.writes[0].offset == 0x10);
    CHECK(h.dev.writes[0].width == 2);
    CHECK(h.dev.writes[0].value == 0x1234);
    REQUIRE(h.dev.reads.size() == 2);
    CHECK(h.dev.reads[0].offset == 0x20);
    CHECK(h.dev.reads[1].offset == 0x22);
    CHECK(h.dev.reads[1].width == 2);
}

TEST_CASE("iop hw: a deferred transfer ends when the device says so", "[iop][hw]") {
    DmaModule mod;
    HwRig h(mod.b);
    h.dev.deferred = true;
    h.load();
    h.run_us(2000);
    const std::uint32_t seen = h.base() + mod.b.offset_of(mod.seen);
    CHECK(dq8::test::iop_ram32(*h.rig.iop, seen) == 0);
    CHECK(h.log(mod.b, mod.log).size() == 3);
    h.dev.host->dma_complete(4, h.rig.m.clock().now());
    h.run_us(100);
    CHECK(dq8::test::iop_ram32(*h.rig.iop, seen) == 0x77);
    CHECK(h.log(mod.b, mod.log).size() == 5);
    CHECK(h.dev.stops.empty());
}

TEST_CASE("iop hw: SSBUS registers are stores the guest reads back", "[iop][hw]") {
    IrxBuilder b("HWTEST");
    const Label log = b.zeros(32);
    const Label start = b.here();
    b.enter(32);
    reg_page(b);
    b.li(m::t1, 0x200B31E1);
    b.emit(m::sw(m::t1, 0x1014, m::t0));
    b.li(m::t1, 0x0000BEEF);
    b.emit(m::sw(m::t1, 0x1404, m::t0));
    b.li(m::t1, 0x12);
    b.emit(m::sb(m::t1, 0x1405, m::t0));
    b.emit(m::lw(m::t2, 0x1014, m::t0));
    b.emit(m::nop());
    b.append(log, m::t2);
    reg_page(b);
    b.emit(m::lw(m::t2, 0x1404, m::t0));
    b.emit(m::nop());
    b.append(log, m::t2);
    b.li(m::v0, 0);
    b.leave(32);
    b.entry(start);
    HwRig h(b);
    h.load();
    CHECK(h.log(b, log) == std::vector<std::uint32_t>{0x200B31E1u, 0x000012EFu});
}

TEST_CASE("iop hw: the HLE page reads as trap words and a linked stub jumps into it", "[iop][hw]") {
    IrxBuilder b("HWTEST");
    const Label log = b.zeros(32);
    b.imports("thbase", {20});
    const Label start = b.here();
    b.enter(32);
    b.emit(m::lui(m::t0, 0x0FF0));
    b.emit(m::lw(m::t1, 8, m::t0));
    b.emit(m::lw(m::t2, 12, m::t0));
    b.emit(m::nop());
    b.append(log, m::t1);
    b.append(log, m::t2);
    b.load(m::t1, b.stub("thbase", 20)); // the linked stub: j into the HLE page
    b.append(log, m::t1);
    b.li(m::v0, 0);
    b.leave(32);
    b.entry(start);
    HwRig h(b);
    h.load();
    const auto v = h.log(b, log);
    REQUIRE(v.size() == 3);
    CHECK(v[0] == (rt::iop_hle_trap_opcode | 1u));
    CHECK(v[1] == 0);
    CHECK((v[2] >> 26) == 2); // j
    CHECK(((v[2] & 0x03FFFFFFu) << 2) >= (rt::iop_map::hle_page & 0x0FFFFFFFu));
}

TEST_CASE("iop hw: the spin detector moves the IOP to the next event", "[iop][hw]") {
    IrxBuilder b("HWTEST");
    const Label stamp = b.zeros(8);
    b.imports("thbase", {4, 6, 34});
    const Label body = b.label();
    start_spawning(b, body);
    b.bind(body);
    b.enter(32);
    b.emit(m::lui(m::t0, 0x1F90));
    const Label poll = b.here();
    b.emit(ia::lhu(m::t1, 0x40, m::t0));
    b.emit(m::nop());
    b.beq(m::t1, m::zero, poll);
    b.la(m::a0, stamp);
    b.jal(b.stub("thbase", 34));
    b.leave(32);
    HwRig h(b);
    const rt::Cycles ready = h.rig.m.clock().now() + rt::cycles_from_us(10'000);
    h.dev.ready_at(ready);
    h.load();
    h.run_us(12'000);
    const std::uint32_t ticks = dq8::test::iop_ram32(*h.rig.iop, h.base() + b.offset_of(stamp));
    CHECK(ticks >= 300'000); // about 10 ms of the IOP clock
    CHECK(h.dev.ready);
    CHECK(h.dev.reads.size() < 500); // 10 ms of polling would be about 90,000 reads
}

TEST_CASE("iop hw: what the IOP bus does not model stops the guest", "[iop][hw]") {
    SECTION("an unmapped register") {
        IrxBuilder b("HWTEST");
        const Label start = b.here();
        reg_page(b);
        b.emit(m::lw(m::t1, 0x1100, m::t0)); // the IOP's timer 0: timrman is HLE
        b.ret();
        b.entry(start);
        HwRig h(b);
        const std::string msg = h.load_until_stop();
        CHECK(h.reason == rt::StopReason::unsupported);
        CHECK_THAT(msg, ContainsSubstring("a load from an unmapped address"));
        CHECK_THAT(msg, ContainsSubstring("unmapped IOP address 0x1F801100"));
        CHECK_THAT(msg, ContainsSubstring("HWTEST+0x"));
    }
    SECTION("a device that refuses an access") {
        IrxBuilder b("HWTEST");
        const Label start = b.here();
        b.emit(m::lui(m::t0, 0x1F90));
        b.emit(m::sw(m::zero, 0x200, m::t0));
        b.ret();
        b.entry(start);
        HwRig h(b);
        CHECK_THAT(h.load_until_stop(), ContainsSubstring("FAKEDEV+0x00000200"));
    }
    SECTION("a halfword store to CHCR") {
        IrxBuilder b("HWTEST");
        const Label start = b.here();
        reg_page(b);
        b.emit(ia::sh(m::zero, 0x10C8, m::t0));
        b.ret();
        b.entry(start);
        HwRig h(b);
        CHECK_THAT(h.load_until_stop(), ContainsSubstring("a 2-byte store to DMA4 CHCR"));
    }
    SECTION("a channel no device serves") {
        IrxBuilder b("HWTEST");
        const Label start = b.here();
        reg_page(b);
        b.li(m::t1, 0x01000201);
        b.emit(m::sw(m::t1, 0x10A8, m::t0)); // channel 2
        b.ret();
        b.entry(start);
        HwRig h(b);
        CHECK_THAT(h.load_until_stop(), ContainsSubstring("IOP DMA channel 2 was started"));
    }
    SECTION("a channel DPCR leaves disabled") {
        IrxBuilder b("HWTEST");
        const Label start = b.here();
        reg_page(b);
        b.li(m::t1, 0x01000201);
        b.emit(m::sw(m::t1, 0x1508, m::t0)); // channel 7
        b.ret();
        b.entry(start);
        HwRig h(b);
        CHECK_THAT(h.load_until_stop(),
                   ContainsSubstring("channel 7 was started while DPCR2 leaves it disabled"));
    }
    SECTION("an enabled interrupt line with no handler") {
        IrxBuilder b("HWTEST");
        b.imports("intrman", {6});
        const Label start = b.here();
        b.enter(32);
        b.call(b.stub("intrman", 6), {9});
        b.li(m::v0, 0);
        b.leave(32);
        b.entry(start);
        HwRig h(b);
        h.load();
        h.dev.host->raise_irq(rt::IopIrq::spu2, h.rig.m.clock().now());
        try {
            h.run_us(100);
            FAIL("no stop");
        } catch (const rt::GuestStop& s) {
            CHECK_THAT(s.info().message,
                       ContainsSubstring("interrupt line 9 was raised while enabled, with no handler"));
        }
    }
}

TEST_CASE("iop hw: a masked interrupt from an idle IOP schedules nothing", "[iop][hw]") {
    IrxBuilder b("HWTEST");
    const Label start = b.here();
    b.li(m::v0, 0);
    b.ret();
    b.entry(start);
    HwRig h(b);
    h.load();
    const std::uint64_t slices = h.rig.iop->interpreter().slices();
    h.dev.host->raise_irq(rt::IopIrq::spu2, h.rig.m.clock().now());
    h.run_us(100);
    CHECK(h.rig.iop->interpreter().slices() == slices);
    for (const auto& e : h.rig.m.events().pending()) {
        CHECK(e.tag != "iop_slice");
    }
}

TEST_CASE("iop hw: device mappings may not overlap or share a channel", "[iop][hw]") {
    FakeDevice a;
    FakeDevice b;
    const auto rig_with = [&](rt::IopDeviceMapping second) {
        IopRig rig(dq8::test::test_disc(), dq8::test::test_modules(), "MODULES/IOPRP.IMG",
                   [&](rt::IopConfig& c) {
                       c.devices.push_back(a.mapping());
                       c.devices.push_back(second);
                   });
    };
    rt::IopDeviceMapping overlap = b.mapping();
    overlap.base = 0x1F900400u;
    overlap.dma_channels.clear();
    CHECK_THROWS_AS(rig_with(overlap), std::invalid_argument);
    rt::IopDeviceMapping shared = b.mapping();
    shared.base = 0x1F910000u;
    shared.dma_channels = {7};
    CHECK_THROWS_AS(rig_with(shared), std::invalid_argument);
    rt::IopDeviceMapping fine = b.mapping();
    fine.base = 0x1F910000u;
    fine.dma_channels = {2};
    CHECK_NOTHROW(rig_with(fine));
}
