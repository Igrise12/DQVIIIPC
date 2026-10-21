// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The DMAC on synthetic chains (Docs/milestones/M3-boots.md, session B "Done when": every tag). SPR_TO copies
// whatever it is given into the scratchpad verbatim, which makes it the plainest witness of a source chain.
#include "runtime/ee/guest_stop.h"
#include "runtime/machine/interrupts.h"
#include "support/ee_rig.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

using namespace dq8::rt;
using dq8::test::dma_tag;
using dq8::test::EeRig;
using dq8::test::gif_tag;
using dq8::test::vif_code;
using dq8::test::write_qword;

namespace {

constexpr std::uint32_t spr_from = 0x1000D000u;
constexpr std::uint32_t spr_to = 0x1000D400u;
constexpr std::uint32_t gif_ch = 0x1000A000u;
constexpr std::uint32_t vif1_ch = 0x10009000u;
constexpr std::uint32_t d_stat = 0x1000E010u;
constexpr std::uint32_t chcr = 0x00u, madr = 0x10u, qwc = 0x20u, tadr = 0x30u, asr0 = 0x40u, asr1 = 0x50u,
                        sadr = 0x80u;
constexpr std::uint32_t str = 0x100u;
constexpr std::uint32_t chain = 0x4u;

void fill_qword(std::uint8_t* at, std::uint8_t value) {
    std::memset(at, value, 16);
}

bool qword_is(const std::uint8_t* at, std::uint8_t value) {
    for (unsigned i = 0; i < 16; ++i) {
        if (at[i] != value) {
            return false;
        }
    }
    return true;
}

void drain(EeRig& rig) {
    rig.run_for(1u << 24);
}

} // namespace

TEST_CASE("dmac: a source chain through every tag id, two levels of call and ret", "[dmac]") {
    EeRig rig;
    std::uint8_t* ram = rig.mem().rdram();
    write_qword(ram + 0x10000, dma_tag(1, 1)); // cnt: data follows, next tag after it
    fill_qword(ram + 0x10010, 0x01);
    write_qword(ram + 0x10020, dma_tag(1, 2, 0x10100)); // next: data follows, next tag at ADDR
    fill_qword(ram + 0x10030, 0x02);
    write_qword(ram + 0x10100, dma_tag(1, 3, 0x20000)); // ref: data at ADDR
    fill_qword(ram + 0x20000, 0x03);
    write_qword(ram + 0x10110, dma_tag(1, 4, 0x20010)); // refs: data at ADDR (stall control unmodelled)
    fill_qword(ram + 0x20010, 0x04);
    write_qword(ram + 0x10120, dma_tag(1, 5, 0x10200)); // call: push 0x10140
    fill_qword(ram + 0x10130, 0x05);
    write_qword(ram + 0x10200, dma_tag(0, 5, 0x10300)); // call with no data: push 0x10210
    write_qword(ram + 0x10300, dma_tag(1, 6));          // ret -> 0x10210
    fill_qword(ram + 0x10310, 0x06);
    write_qword(ram + 0x10210, dma_tag(1, 6)); // ret -> 0x10140
    fill_qword(ram + 0x10220, 0x07);
    write_qword(ram + 0x10140, dma_tag(1, 0, 0x20020)); // refe: data at ADDR, then end
    fill_qword(ram + 0x20020, 0x08);
    write_qword(ram + 0x10150, dma_tag(1, 1)); // never reached
    fill_qword(ram + 0x10160, 0xEE);

    rig.w32(spr_to + sadr, 0);
    rig.w32(spr_to + tadr, 0x10000);
    rig.w32(spr_to + chcr, str | chain);
    const std::uint8_t* spr = rig.mem().scratchpad();
    for (std::uint8_t i = 0; i < 8; ++i) {
        INFO("quadword " << int{i});
        CHECK(qword_is(spr + i * 16u, static_cast<std::uint8_t>(i + 1)));
    }
    CHECK(qword_is(spr + 8 * 16u, 0x00)); // nothing after refe
    CHECK(rig.r32(spr_to + sadr) == 8u * 16u);
    CHECK(rig.r32(spr_to + tadr) == 0x10150u); // refe: TADR moves past the tag
    CHECK(rig.r32(spr_to + asr0) == 0x10140u);
    CHECK(rig.r32(spr_to + asr1) == 0x10210u);
    const std::uint32_t c = rig.r32(spr_to + chcr);
    CHECK(((c >> 4) & 3u) == 0u);  // ASP back to 0
    CHECK(((c >> 28) & 7u) == 0u); // TAG: the last tag was refe
    CHECK((c & str) != 0u);        // busy until the completion event
    CHECK(rig.dmac().stats(DmaChannel::spr_to).tags == 9u);
    CHECK(rig.dmac().stats(DmaChannel::spr_to).qwords == 8u);
    drain(rig);
    CHECK((rig.r32(spr_to + chcr) & str) == 0u);
    CHECK((rig.r32(d_stat) & (1u << 9)) != 0u);
}

TEST_CASE("dmac: end, ret on an empty stack, IRQ with TIE, and a QWC left over from the last tag", "[dmac]") {
    SECTION("end transfers the data after the tag and stops") {
        EeRig rig;
        std::uint8_t* ram = rig.mem().rdram();
        write_qword(ram + 0x1000, dma_tag(1, 7));
        fill_qword(ram + 0x1010, 0x31);
        fill_qword(ram + 0x1020, 0xEE);
        rig.w32(spr_to + tadr, 0x1000);
        rig.w32(spr_to + chcr, str | chain);
        CHECK(qword_is(rig.mem().scratchpad(), 0x31));
        CHECK(qword_is(rig.mem().scratchpad() + 16, 0x00));
        CHECK(((rig.r32(spr_to + chcr) >> 28) & 7u) == 7u);
    }
    SECTION("ret with ASP 0 ends the chain") {
        EeRig rig;
        std::uint8_t* ram = rig.mem().rdram();
        write_qword(ram + 0x1000, dma_tag(1, 6));
        fill_qword(ram + 0x1010, 0x32);
        write_qword(ram + 0x1020, dma_tag(1, 1));
        fill_qword(ram + 0x1030, 0xEE);
        rig.w32(spr_to + tadr, 0x1000);
        rig.w32(spr_to + chcr, str | chain);
        CHECK(qword_is(rig.mem().scratchpad(), 0x32));
        CHECK(qword_is(rig.mem().scratchpad() + 16, 0x00));
    }
    SECTION("an IRQ tag stops the chain after its data only with TIE") {
        for (const bool tie : {false, true}) {
            EeRig rig;
            std::uint8_t* ram = rig.mem().rdram();
            write_qword(ram + 0x1000, dma_tag(1, 1, 0, true));
            fill_qword(ram + 0x1010, 0x33);
            write_qword(ram + 0x1020, dma_tag(1, 7));
            fill_qword(ram + 0x1030, 0x34);
            rig.w32(spr_to + tadr, 0x1000);
            rig.w32(spr_to + chcr, str | chain | (tie ? 0x80u : 0u));
            CHECK(qword_is(rig.mem().scratchpad(), 0x33));
            CHECK(qword_is(rig.mem().scratchpad() + 16, tie ? 0x00 : 0x34));
            CHECK(((rig.r32(spr_to + chcr) >> 31) & 1u) == (tie ? 1u : 0u)); // TAG.IRQ of the last tag
        }
    }
    SECTION("QWC > 0 at the start goes first; the TAG field written decides whether the chain goes on") {
        for (const std::uint32_t last_id : {1u, 0u}) { // cnt continues, refe stops
            EeRig rig;
            std::uint8_t* ram = rig.mem().rdram();
            fill_qword(ram + 0x3000, 0x35);
            write_qword(ram + 0x1000, dma_tag(1, 7));
            fill_qword(ram + 0x1010, 0x36);
            rig.w32(spr_to + madr, 0x3000);
            rig.w32(spr_to + qwc, 1);
            rig.w32(spr_to + tadr, 0x1000);
            rig.w32(spr_to + chcr, str | chain | (last_id << 28));
            CHECK(qword_is(rig.mem().scratchpad(), 0x35));
            CHECK(qword_is(rig.mem().scratchpad() + 16, last_id == 1 ? 0x36 : 0x00));
            CHECK(rig.r32(spr_to + qwc) == 0u);
        }
    }
}

TEST_CASE("dmac: TTE sends the whole tag to the scratchpad and the upper half to a VIF", "[dmac]") {
    EeRig rig;
    std::uint8_t* ram = rig.mem().rdram();
    write_qword(ram + 0x1000, dma_tag(1, 7), 0x1122334455667788ull);
    fill_qword(ram + 0x1010, 0x41);
    rig.w32(spr_to + tadr, 0x1000);
    rig.w32(spr_to + chcr, str | chain | 0x40u);
    std::uint64_t hi = 0;
    std::memcpy(&hi, rig.mem().scratchpad() + 8, 8);
    CHECK(hi == 0x1122334455667788ull);
    CHECK(qword_is(rig.mem().scratchpad() + 16, 0x41));

    // VIF1: the tag's upper half is two VIFcodes (here STMOD 2 and ITOP 0x12).
    write_qword(ram + 0x2000, dma_tag(0, 7),
                std::uint64_t{vif_code(0x05, 0, 2)} | (std::uint64_t{vif_code(0x04, 0, 0x12)} << 32));
    rig.w32(vif1_ch + tadr, 0x2000);
    rig.w32(vif1_ch + chcr, str | chain | 0x40u | 1u);
    CHECK(rig.vif1().registers().mode == 2u);
    CHECK(rig.vif1().registers().itops == 0x12u);
}

TEST_CASE("dmac: completion is an event dma_per_qword cycles per quadword after the start", "[dmac]") {
    EeRig rig;
    rig.mem().set_mmio_bus(&rig.machine.mmio(), &rig.machine.clock(), 0); // accesses cost nothing here
    for (unsigned i = 0; i < 4; ++i) {
        fill_qword(rig.mem().rdram() + 0x1000 + 16 * i, static_cast<std::uint8_t>(0x50 + i));
    }
    rig.w32(spr_to + madr, 0x1000);
    rig.w32(spr_to + qwc, 4);
    const Cycles start = rig.machine.clock().now();
    rig.w32(spr_to + chcr, str);
    CHECK(qword_is(rig.mem().scratchpad() + 48, 0x53)); // delivered at once
    const Cycles done = start + 4 * rig.machine.cost().dma_per_qword;
    rig.machine.advance(done - 1);
    CHECK((rig.r32(spr_to + chcr) & str) != 0u);
    CHECK((rig.machine.interrupts().dmac_stat() & (1u << 9)) == 0u);
    rig.machine.advance(done);
    CHECK((rig.r32(spr_to + chcr) & str) == 0u);
    CHECK((rig.machine.interrupts().dmac_stat() & (1u << 9)) != 0u);
    CHECK(rig.r32(spr_to + madr) == 0x1040u);

    SECTION("an empty transfer still completes later, never at once") {
        rig.w32(spr_to + qwc, 0);
        rig.machine.interrupts().write_dmac_stat(1u << 9);
        rig.w32(spr_to + chcr, str);
        CHECK((rig.r32(spr_to + chcr) & str) != 0u);
        drain(rig);
        CHECK((rig.r32(spr_to + chcr) & str) == 0u);
    }
    SECTION("clearing STR cancels the completion") {
        rig.machine.interrupts().write_dmac_stat(1u << 9);
        rig.w32(spr_to + qwc, 1);
        rig.w32(spr_to + chcr, str);
        rig.w32(spr_to + chcr, 0);
        drain(rig);
        CHECK((rig.machine.interrupts().dmac_stat() & (1u << 9)) == 0u);
    }
}

TEST_CASE("dmac: a stalled VIF holds the channel's completion until FBRST.STC", "[dmac][vif]") {
    EeRig rig;
    std::uint8_t* ram = rig.mem().rdram();
    std::vector<std::uint8_t> data;
    dq8::test::put32(data, vif_code(0x07, 0, 0x99, true)); // MARK with the i bit: stall after it
    dq8::test::put32(data, vif_code(0x05, 0, 1));          // STMOD 1, held while stalled
    dq8::test::put32(data, 0);
    dq8::test::put32(data, 0);
    std::memcpy(ram + 0x1000, data.data(), data.size());
    rig.w32(vif1_ch + madr, 0x1000);
    rig.w32(vif1_ch + qwc, 1);
    rig.w32(vif1_ch + chcr, str | 1u);
    CHECK(rig.vif1().stalled());
    CHECK(rig.vif1().holding_input());
    CHECK(rig.vif1().registers().mode == 0u);
    drain(rig);
    CHECK((rig.r32(vif1_ch + chcr) & str) != 0u); // held
    CHECK((rig.machine.interrupts().intc_stat() & (1u << static_cast<unsigned>(IntcLine::vif1))) != 0u);
    rig.w32(0x10003C10u, 0x8u); // FBRST.STC
    CHECK(rig.vif1().registers().mode == 1u);
    CHECK_FALSE(rig.vif1().holding_input());
    drain(rig);
    CHECK((rig.r32(vif1_ch + chcr) & str) == 0u);
    CHECK((rig.r32(d_stat) & 2u) != 0u);
}

TEST_CASE("dmac: a masked PATH3 holds the GIF channel's completion until the mask lifts", "[dmac][gif]") {
    EeRig rig;
    rig.vif1().push(std::vector<std::uint8_t>{0x00, 0x80, 0x00, 0x06}); // MSKPATH3 on
    write_qword(rig.mem().rdram() + 0x1000, gif_tag(0, 0, 1));
    rig.w32(gif_ch + madr, 0x1000);
    rig.w32(gif_ch + qwc, 1);
    rig.w32(gif_ch + chcr, str);
    drain(rig);
    CHECK(rig.sink.transfers.empty());
    CHECK((rig.r32(gif_ch + chcr) & str) != 0u);
    rig.vif1().push(std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x06}); // MSKPATH3 off
    CHECK(rig.sink.transfers.size() == 1u);
    drain(rig);
    CHECK((rig.r32(gif_ch + chcr) & str) == 0u);
}

TEST_CASE("dmac: SPR_FROM destination chains with cnt, cnts and end", "[dmac]") {
    EeRig rig;
    std::uint8_t* spr = rig.mem().scratchpad();
    write_qword(spr + 0x00, dma_tag(1, 1, 0x30000)); // cnt
    fill_qword(spr + 0x10, 0x61);
    write_qword(spr + 0x20, dma_tag(2, 0, 0x30100)); // cnts
    fill_qword(spr + 0x30, 0x62);
    fill_qword(spr + 0x40, 0x63);
    write_qword(spr + 0x50, dma_tag(1, 7, 0x30200)); // end
    fill_qword(spr + 0x60, 0x64);
    write_qword(spr + 0x70, dma_tag(1, 1, 0x30300));
    fill_qword(spr + 0x80, 0xEE);
    rig.w32(spr_from + sadr, 0);
    rig.w32(spr_from + chcr, str | chain);
    const std::uint8_t* ram = rig.mem().rdram();
    CHECK(qword_is(ram + 0x30000, 0x61));
    CHECK(qword_is(ram + 0x30100, 0x62));
    CHECK(qword_is(ram + 0x30110, 0x63));
    CHECK(qword_is(ram + 0x30200, 0x64));
    CHECK(qword_is(ram + 0x30300, 0x00));
    CHECK(rig.r32(spr_from + sadr) == 0x70u);
    CHECK(rig.r32(spr_from + madr) == 0x30210u);
    drain(rig);
    CHECK((rig.r32(d_stat) & (1u << 8)) != 0u);

    SECTION("a source id in a destination chain stops") {
        EeRig bad;
        write_qword(bad.mem().scratchpad(), dma_tag(1, 3, 0x30000));
        CHECK_THROWS_AS(bad.w32(spr_from + chcr, str | chain), GuestStop);
    }
}

TEST_CASE("dmac: scratchpad interleave transfers TQWC and skips SQWC in memory", "[dmac]") {
    EeRig rig;
    rig.w32(0x1000E030u, (1u << 16) | 1u); // D_SQWC: TQWC 1, SQWC 1
    std::uint8_t* spr = rig.mem().scratchpad();
    for (unsigned i = 0; i < 3; ++i) {
        fill_qword(spr + 16 * i, static_cast<std::uint8_t>(0x71 + i));
    }
    rig.w32(spr_from + madr, 0x40000);
    rig.w32(spr_from + qwc, 3);
    rig.w32(spr_from + chcr, str | 0x8u); // MOD 2
    const std::uint8_t* ram = rig.mem().rdram();
    CHECK(qword_is(ram + 0x40000, 0x71));
    CHECK(qword_is(ram + 0x40010, 0x00));
    CHECK(qword_is(ram + 0x40020, 0x72));
    CHECK(qword_is(ram + 0x40040, 0x73));

    std::uint8_t* wram = rig.mem().rdram();
    for (unsigned i = 0; i < 4; ++i) {
        fill_qword(wram + 0x50000 + 16 * i, static_cast<std::uint8_t>(0x81 + i));
    }
    rig.w32(spr_to + madr, 0x50000);
    rig.w32(spr_to + qwc, 2);
    rig.w32(spr_to + sadr, 0x100);
    rig.w32(spr_to + chcr, str | 0x8u);
    CHECK(qword_is(spr + 0x100, 0x81));
    CHECK(qword_is(spr + 0x110, 0x83));
}

TEST_CASE("dmac: DMAE and D_ENABLEW.CPND hold a start until they allow it", "[dmac]") {
    EeRig rig;
    CHECK((rig.r32(0x1000E000u) & 1u) == 1u); // DMAE on at power-on (the BIOS's state)
    CHECK(rig.r32(0x1000F520u) == 0x1201u);
    fill_qword(rig.mem().rdram() + 0x1000, 0x91);
    rig.w32(0x1000F590u, 0x1201u | 0x10000u); // suspend
    rig.w32(spr_to + madr, 0x1000);
    rig.w32(spr_to + qwc, 1);
    rig.w32(spr_to + chcr, str);
    CHECK(qword_is(rig.mem().scratchpad(), 0x00));
    CHECK(rig.r32(0x1000F520u) == 0x11201u);
    rig.w32(0x1000F590u, 0x1201u);
    CHECK(qword_is(rig.mem().scratchpad(), 0x91));
}

TEST_CASE("dmac: SIF0 and SIF1 store their registers and move nothing", "[dmac]") {
    EeRig rig;
    for (const std::uint32_t ch : {0x1000C000u, 0x1000C400u}) {
        rig.w32(ch + madr, 0x2000);
        rig.w32(ch + qwc, 3);
        rig.w32(ch + tadr, 0x3000);
        rig.w32(ch + chcr, str | chain);
        drain(rig);
        CHECK(rig.r32(ch + madr) == 0x2000u);
        CHECK(rig.r32(ch + qwc) == 3u);
        CHECK(rig.r32(ch + tadr) == 0x3000u);
        CHECK((rig.r32(ch + chcr) & str) != 0u); // the IOP link completes SIF transfers
    }
    CHECK(rig.r32(d_stat) == 0u);
}

TEST_CASE("dmac: unmodelled channels and bad chains stop with a diagnostic", "[dmac]") {
    SECTION("IPU") {
        EeRig rig;
        rig.w32(0x1000B400u + qwc, 1);
        try {
            rig.w32(0x1000B400u + chcr, str);
            FAIL("no stop");
        } catch (const GuestStop& stop) {
            CHECK(stop.info().reason == StopReason::unsupported);
            CHECK(stop.info().message.find("IPU") != std::string::npos);
        }
    }
    SECTION("an address outside RAM and the scratchpad") {
        EeRig rig;
        rig.w32(spr_to + madr, 0x0A000000u);
        rig.w32(spr_to + qwc, 1);
        CHECK_THROWS_AS(rig.w32(spr_to + chcr, str), GuestStop);
    }
    SECTION("a chain that never ends") {
        EeRig rig;
        write_qword(rig.mem().rdram() + 0x1000, dma_tag(0, 2, 0x1000)); // next -> itself
        rig.w32(spr_to + tadr, 0x1000);
        CHECK_THROWS_AS(rig.w32(spr_to + chcr, str | chain), GuestStop);
    }
    SECTION("a third call") {
        EeRig rig;
        std::uint8_t* ram = rig.mem().rdram();
        write_qword(ram + 0x1000, dma_tag(0, 5, 0x1100));
        write_qword(ram + 0x1100, dma_tag(0, 5, 0x1200));
        write_qword(ram + 0x1200, dma_tag(0, 5, 0x1300));
        rig.w32(spr_to + tadr, 0x1000);
        CHECK_THROWS_AS(rig.w32(spr_to + chcr, str | chain), GuestStop);
    }
}

TEST_CASE("dmac: VIF1 to memory reads GS data through the sink", "[dmac][gif]") {
    EeRig rig;
    rig.w32(vif1_ch + madr, 0x6000);
    rig.w32(vif1_ch + qwc, 2);
    rig.w32(vif1_ch + chcr, str); // DIR 0: VIF1 -> memory
    const std::uint8_t* ram = rig.mem().rdram();
    for (unsigned i = 0; i < 32; ++i) {
        CHECK(ram[0x6000 + i] == static_cast<std::uint8_t>(i));
    }
    CHECK(rig.sink.reads == 1u);
}

TEST_CASE("dmac: DMA starts and GIF packets are traced when their categories are on", "[dmac][gif]") {
    EeRig rig;
    rig.machine.trace_mask = trace_bit(TraceCategory::dma) | trace_bit(TraceCategory::gs);
    write_qword(rig.mem().rdram() + 0x1000, gif_tag(0, 0, 1));
    rig.w32(gif_ch + madr, 0x1000);
    rig.w32(gif_ch + qwc, 1);
    rig.w32(gif_ch + chcr, str);
    std::vector<std::string> lines;
    for (const MachineEvent& e : rig.machine.emitted) {
        if (e.type == "trace") {
            lines.push_back(std::get<std::string>(e.fields[0].second) + ": " +
                            std::get<std::string>(e.fields[1].second));
        }
    }
    REQUIRE(lines.size() == 2u);
    CHECK(lines[0] == "gs: path3 packet 16 bytes");
    CHECK(lines[1] == "dma: gif mode 0 chcr 0x00000100 tadr 0x00000000: 1 qwords, 0 tags");
}
