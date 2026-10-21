// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xTest/src/ps2_memory_tests.cpp. The cases keep upstream's names, setups and expected values; the harness
// is rewritten for the M3 device model (make_ee_devices on a FakeMachine, guest accesses through the MMIO
// bus, completions as events). Where our model differs on purpose the case says how:
//   - the GIF cuts whole packets at EOP, so upstream's raw byte payloads are given valid GIFtags (an
//     EOP-only tag whose upper half carries the pattern, or an IMAGE tag) and the checks look at packets;
//   - DMA completion is an event: "after drain" means after the machine runs the completion event;
//   - upstream's GS model, GIF arbiter, native GIF fast paths, by-name library fakes (sceDma*, sceVif1Pk*),
//     its PS2Memory-only helpers (translateAddress, the 0xF0000000 scratchpad alias, unaligned-access
//     exceptions) and its raw-continuation quirk after DIRECT do not exist here; those cases are listed at
//     the end of this file with the reason, and the behaviour we have instead is checked where it applies.
#include "dq8/abi/ps2recomp/ps2_runtime_macros.h"
#include "runtime/machine/interrupts.h"
#include "support/ee_rig.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace dq8::rt;
using dq8::test::EeRig;
using dq8::test::put32;
using dq8::test::put64;

namespace {

constexpr uint32_t kGifCh = 0x1000A000u;
constexpr uint32_t kVif1Ch = 0x10009000u;
constexpr uint32_t kDStat = 0x1000E010u;
constexpr uint32_t kDctrl = 0x1000E000u;
constexpr uint32_t kGifStat = 0x10003020u;
constexpr uint8_t GIF_FMT_PACKED = 0u;
constexpr uint8_t GIF_FMT_IMAGE = 2u;
constexpr uint64_t GS_REG_BITBLTBUF = 0x50u;
constexpr uint64_t GS_REG_TRXPOS = 0x51u;
constexpr uint64_t GS_REG_TRXREG = 0x52u;
constexpr uint64_t GS_REG_TRXDIR = 0x53u;

uint32_t makeVifCmd(uint8_t opcode, uint8_t num, uint16_t imm) {
    return (static_cast<uint32_t>(opcode) << 24) | (static_cast<uint32_t>(num) << 16) |
           static_cast<uint32_t>(imm);
}

uint64_t makeDmaTag(uint16_t qwc, uint8_t id, uint32_t addr, bool irq = false) {
    return static_cast<uint64_t>(qwc) | (static_cast<uint64_t>(id & 0x7u) << 28) |
           (irq ? (1ull << 31) : 0ull) | (static_cast<uint64_t>(addr) << 32);
}

uint64_t makeGifTag(uint16_t nloop, uint8_t flg, uint8_t nreg, bool eop = true) {
    uint64_t tag = static_cast<uint64_t>(nloop & 0x7FFFu);
    if (eop) {
        tag |= (1ull << 15);
    }
    tag |= (static_cast<uint64_t>(flg & 0x3u) << 58);
    tag |= (static_cast<uint64_t>(nreg & 0xFu) << 60);
    return tag;
}

void writeU64(uint8_t* base, uint32_t addr, uint64_t value) {
    std::memcpy(base + addr, &value, sizeof(value));
}

void writeDmaTag(uint8_t* base, uint32_t tagAddr, uint64_t tagLo) {
    std::memset(base + tagAddr, 0, 16);
    writeU64(base, tagAddr, tagLo);
}

// A complete one-quadword GIF packet (an EOP tag with no data) whose upper half is `b0, b0+1, ...` from
// byte 8 on, and whose first eight bytes are the tag: the stand-in for upstream's raw 16-byte payloads.
void writeEopQword(uint8_t* base, uint32_t addr, uint8_t b0) {
    writeU64(base, addr, makeGifTag(0u, GIF_FMT_PACKED, 1u, true));
    for (uint32_t i = 8; i < 16u; ++i) {
        base[addr + i] = static_cast<uint8_t>(b0 + i);
    }
}
bool isEopQword(const std::vector<uint8_t>& packet, uint8_t b0) {
    if (packet.size() != 16u) {
        return false;
    }
    for (uint32_t i = 8; i < 16u; ++i) {
        if (packet[i] != static_cast<uint8_t>(b0 + i)) {
            return false;
        }
    }
    return true;
}

// An IMAGE packet of `qwords` data quadwords (tag first) in memory; data bytes are b0, b0+1, ...
void writeImagePacket(uint8_t* base, uint32_t addr, uint16_t qwords, uint8_t b0) {
    writeU64(base, addr, makeGifTag(qwords, GIF_FMT_IMAGE, 0u, true));
    writeU64(base, addr + 8u, 0u);
    for (uint32_t i = 0; i < qwords * 16u; ++i) {
        base[addr + 16u + i] = static_cast<uint8_t>(b0 + i);
    }
}

// Runs the pending DMA completion events.
void drain(EeRig& rig) {
    rig.run_for(1u << 24);
}

std::vector<uint8_t> words(std::initializer_list<uint32_t> list) {
    std::vector<uint8_t> out;
    for (const uint32_t w : list) {
        put32(out, w);
    }
    return out;
}

} // namespace

TEST_CASE("mmio devices: uncached aliases map to same RDRAM bytes", "[mmio][memory]") {
    EeRig rig;
    GuestMemory& mem = rig.mem();
    mem.store32(0x00001000u, 0xDEADBEEFu);
    CHECK(mem.load32(0x00001000u) == 0xDEADBEEFu); // base readback should match
    CHECK(mem.load32(0x20001000u) == 0xDEADBEEFu); // 0x2000_0000 alias should map to RDRAM
    mem.store32(0x00101000u, 0xDEADBEEFu);
    CHECK(mem.load32(0x30101000u) == 0xDEADBEEFu); // 0x3010_0000 accelerated alias should map to RDRAM
    mem.store32(0x20002000u, 0x13579BDFu);
    CHECK(mem.load32(0x00002000u) == 0x13579BDFu); // writes through 0x2000 alias should land in base RDRAM
    mem.store32(0x30103000u, 0x2468ACE0u);
    CHECK(mem.load32(0x00103000u) == 0x2468ACE0u); // writes through 0x3010 alias should land in base RDRAM
}

TEST_CASE("mmio devices: EE timer0 count advances from scheduler cycles and can be reset", "[mmio][timers]") {
    EeRig rig;
    constexpr uint32_t kTimer0Count = 0x10000000u;
    constexpr uint32_t kTimer0Mode = 0x10000010u;
    constexpr uint32_t kTimer0Compare = 0x10000020u;
    rig.w32(kTimer0Count, 0u);
    rig.w32(kTimer0Compare, 0xFFFFu);
    rig.w32(kTimer0Mode, 0x82u);
    CHECK(rig.r32(kTimer0Mode) == 0x82u); // timer mode should be readable

    const Cycles start = rig.machine.clock().now();
    rig.machine.advance(start + 8u * 512u);
    CHECK(rig.r32(kTimer0Count) == 8u); // BUSCLK/256 should increment once per 512 EE cycles

    rig.w32(kTimer0Count, 0u);
    const Cycles reset_at = rig.machine.clock().now();
    // Counting restarts at the reset: one tick is the next multiple of 512 cycles after it.
    rig.machine.advance((reset_at / 512u + 1u) * 512u);
    CHECK(rig.r32(kTimer0Count) == 1u); // timer reset should restart the deterministic count window
}

TEST_CASE("mmio devices: EE timers 0 through 3 expose independent COUNT MODE and COMP registers",
          "[mmio][timers]") {
    EeRig rig;
    constexpr uint32_t kTimerBases[] = {0x10000000u, 0x10000800u, 0x10001000u, 0x10001800u};
    constexpr uint32_t kBusClockDiv256Cue = 0x82u;
    for (uint32_t index = 0u; index < 4u; ++index) {
        const uint32_t base = kTimerBases[index];
        rig.w32(base, 0x100u + index);
        rig.w32(base + 0x10u, kBusClockDiv256Cue);
        rig.w32(base + 0x20u, 0x200u + index);
    }
    // One BUSCLK/256 tick: the next 512-cycle boundary (the writes above stay inside the first window).
    rig.machine.advance((rig.machine.clock().now() / 512u + 1u) * 512u);
    for (uint32_t index = 0u; index < 4u; ++index) {
        const uint32_t base = kTimerBases[index];
        CHECK(rig.r32(base) == 0x101u + index);             // each timer should advance its own COUNT
        CHECK(rig.r32(base + 0x10u) == kBusClockDiv256Cue); // each timer should retain MODE
        CHECK(rig.r32(base + 0x20u) == 0x200u + index);     // each timer should retain COMP
    }
    rig.w32(kTimerBases[0] + 0x30u, 0x12345u);
    rig.w32(kTimerBases[1] + 0x30u, 0x23456u);
    CHECK(rig.r32(kTimerBases[0] + 0x30u) == 0x2345u); // Timer0 HOLD should be 16-bit
    CHECK(rig.r32(kTimerBases[1] + 0x30u) == 0x3456u); // Timer1 HOLD should be 16-bit
}

TEST_CASE("mmio devices: EE Timer2 compare and overflow flags raise INTC_TIM2 and clear on write-one",
          "[mmio][timers]") {
    EeRig rig;
    constexpr uint32_t kTimer2Count = 0x10001000u;
    constexpr uint32_t kTimer2Mode = 0x10001010u;
    constexpr uint32_t kTimer2Compare = 0x10001020u;
    constexpr uint32_t kCue = 1u << 7u;
    constexpr uint32_t kCmpe = 1u << 8u;
    constexpr uint32_t kOvfe = 1u << 9u;
    constexpr uint32_t kEquf = 1u << 10u;
    constexpr uint32_t kOvff = 1u << 11u;
    constexpr uint32_t kBusClockDiv256 = 2u;
    constexpr uint32_t kTim2 = 1u << static_cast<unsigned>(IntcLine::timer2);
    InterruptController& intc = rig.machine.interrupts();

    rig.w32(kTimer2Compare, 8u);
    rig.w32(kTimer2Mode, kBusClockDiv256 | kCue | kCmpe | kEquf | kOvff);
    rig.w32(kTimer2Count, 0u);
    const Cycles base = rig.machine.clock().now();
    const Cycles first_tick = (base / 512u + 1u) * 512u;

    rig.machine.advance(first_tick + 6u * 512u);
    CHECK((intc.intc_stat() & kTim2) == 0u); // compare should not fire before COUNT reaches COMP
    CHECK(rig.r32(kTimer2Count) == 7u);      // Timer2 should expose its live 16-bit count
    rig.machine.advance(first_tick + 7u * 512u);
    CHECK((intc.intc_stat() & kTim2) != 0u);     // Timer2 compare should raise the TIM2 interrupt bit
    CHECK((rig.r32(kTimer2Mode) & kEquf) != 0u); // Timer2 compare should latch EQUF

    rig.w32(kTimer2Mode, rig.r32(kTimer2Mode) | kEquf);
    CHECK((rig.r32(kTimer2Mode) & kEquf) == 0u); // writing one should clear EQUF

    intc.write_intc_stat(kTim2);
    rig.w32(kTimer2Mode, kBusClockDiv256 | kCue | kOvfe | kOvff);
    rig.w32(kTimer2Count, 0xFFFFu);
    rig.machine.advance((rig.machine.clock().now() / 512u + 1u) * 512u);
    CHECK((intc.intc_stat() & kTim2) != 0u);     // Timer2 overflow should raise the TIM2 interrupt bit
    CHECK(rig.r32(kTimer2Count) == 0u);          // Timer2 count should wrap at 16 bits
    CHECK((rig.r32(kTimer2Mode) & kOvff) != 0u); // Timer2 overflow should latch OVFF

    rig.w32(kTimer2Mode, rig.r32(kTimer2Mode) | kOvff);
    CHECK((rig.r32(kTimer2Mode) & kOvff) == 0u); // writing one should clear OVFF
}

TEST_CASE("mmio devices: EE timer zero-return clears COUNT on compare", "[mmio][timers]") {
    EeRig rig;
    constexpr uint32_t kTimer0Count = 0x10000000u;
    constexpr uint32_t kTimer0Mode = 0x10000010u;
    constexpr uint32_t kTimer0Compare = 0x10000020u;
    constexpr uint32_t kZret = 1u << 6u;
    constexpr uint32_t kCue = 1u << 7u;
    constexpr uint32_t kCmpe = 1u << 8u;
    constexpr uint32_t kEquf = 1u << 10u;

    // At BUSCLK (2 EE cycles a tick) the cost of the accesses themselves would show; make them free.
    rig.mem().set_mmio_bus(&rig.machine.mmio(), &rig.machine.clock(), 0);
    rig.w32(kTimer0Compare, 3u);
    rig.w32(kTimer0Mode, kZret | kCue | kCmpe | kEquf);
    const Cycles written_at = rig.machine.clock().now();
    rig.w32(kTimer0Count, 0u);
    rig.machine.advance(written_at + 6u); // three BUSCLK ticks
    CHECK((rig.machine.interrupts().intc_stat() & (1u << static_cast<unsigned>(IntcLine::timer0))) != 0u);
    CHECK(rig.r32(kTimer0Count) == 0u); // ZRET should clear COUNT when it equals COMP
}

TEST_CASE("mmio devices: VU0 code and data windows map through EE addresses", "[mmio][vu]") {
    EeRig rig;
    constexpr uint32_t kCodeAddr = 0x11000000u + 0x20u;
    rig.w32(kCodeAddr, 0x11223344u);
    CHECK(rig.r32(kCodeAddr) == 0x11223344u); // VU0 code readback should match written word
    uint32_t codeWord = 0u;
    std::memcpy(&codeWord, rig.vu().micro(0).data() + 0x20u, sizeof(codeWord));
    CHECK(codeWord == 0x11223344u); // VU0 code write should land in micro memory buffer

    constexpr uint32_t kDataAddr = 0x11004000u + 0x30u;
    rig.mem().store128(kDataAddr, 0x89ABCDEFCAFEBABEull, 0x4455667701234567ull);
    uint64_t q[2] = {};
    rig.mem().load128(kDataAddr, q);
    uint32_t lanes[4] = {};
    std::memcpy(lanes, q, sizeof(lanes));
    CHECK(lanes[0] == 0xCAFEBABEu); // VU0 data lane 0 should match
    CHECK(lanes[1] == 0x89ABCDEFu); // VU0 data lane 1 should match
    CHECK(lanes[2] == 0x01234567u); // VU0 data lane 2 should match
    CHECK(lanes[3] == 0x44556677u); // VU0 data lane 3 should match
}

TEST_CASE("mmio devices: fast memory helpers wrap safely at RAM boundary", "[mmio][memory]") {
    std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
    const uint32_t tail = PS2_RAM_SIZE - 4u;
    rdram[tail + 0u] = 0xA1u;
    rdram[tail + 1u] = 0xB2u;
    rdram[tail + 2u] = 0xC3u;
    rdram[tail + 3u] = 0xD4u;
    rdram[0u] = 0x11u;
    rdram[1u] = 0x22u;
    rdram[2u] = 0x33u;
    rdram[3u] = 0x44u;
    CHECK(Ps2FastRead64(rdram.data(), tail) == 0x44332211D4C3B2A1ull); // should wrap across the 32MB boundary
    Ps2FastWrite64(rdram.data(), tail, 0x8877665544332211ull);
    CHECK(rdram[tail + 0u] == 0x11u);
    CHECK(rdram[tail + 1u] == 0x22u);
    CHECK(rdram[tail + 2u] == 0x33u);
    CHECK(rdram[tail + 3u] == 0x44u);
    CHECK(rdram[0u] == 0x55u); // write byte 4 should wrap to address 0
    CHECK(rdram[1u] == 0x66u);
    CHECK(rdram[2u] == 0x77u);
    CHECK(rdram[3u] == 0x88u);
}

TEST_CASE("mmio devices: VIF MPG num zero uploads 256 instructions", "[mmio][vif]") {
    EeRig rig;
    std::vector<uint8_t> packet;
    put32(packet, makeVifCmd(0x4Au, 0u, 0u)); // MPG, num=0 -> 256 instructions (2048 bytes)
    for (uint32_t i = 0; i < 2048u; ++i) {
        packet.push_back(static_cast<uint8_t>(i & 0xFFu));
    }
    rig.vif1().push(packet);
    bool matches = true;
    for (uint32_t i = 0; i < 2048u; ++i) {
        matches = matches && rig.vu().micro(1)[i] == static_cast<uint8_t>(i & 0xFFu);
    }
    CHECK(matches); // MPG num=0 should copy 2048 bytes into VU1 code memory
}

TEST_CASE("mmio devices: VIF UNPACK num zero uploads 256 vectors", "[mmio][vif]") {
    EeRig rig;
    std::vector<uint8_t> packet;
    put32(packet, makeVifCmd(0x6Cu, 0u, 0u)); // UNPACK V4_32, num=0 => 256 vectors
    for (uint32_t i = 0; i < 4096u; ++i) {
        packet.push_back(static_cast<uint8_t>((i * 3u) & 0xFFu));
    }
    rig.vif1().push(packet);
    bool matches = true;
    for (uint32_t i = 0; i < 4096u; ++i) {
        matches = matches && rig.vu().data(1)[i] == static_cast<uint8_t>((i * 3u) & 0xFFu);
    }
    CHECK(matches); // UNPACK num=0 should copy 256 V4_32 vectors (4096 bytes)
}

TEST_CASE("mmio devices: VIF control commands update MARK MASK ROW and COL registers", "[mmio][vif]") {
    EeRig rig;
    rig.vif1().push(
        words({makeVifCmd(0x07u, 0u, 0x1234u),                                                   // MARK
               makeVifCmd(0x20u, 0u, 0u), 0x89ABCDEFu,                                           // STMASK
               makeVifCmd(0x30u, 0u, 0u), 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,    // STROW
               makeVifCmd(0x31u, 0u, 0u), 0xAAAA0001u, 0xAAAA0002u, 0xAAAA0003u, 0xAAAA0004u})); // STCOL
    const Vif::Registers& r = rig.vif1().registers();
    CHECK(r.mark == 0x1234u);
    CHECK(r.mask == 0x89ABCDEFu);
    CHECK(r.row[0] == 0x11111111u);
    CHECK(r.row[1] == 0x22222222u);
    CHECK(r.row[2] == 0x33333333u);
    CHECK(r.row[3] == 0x44444444u);
    CHECK(r.col[0] == 0xAAAA0001u);
    CHECK(r.col[1] == 0xAAAA0002u);
    CHECK(r.col[2] == 0xAAAA0003u);
    CHECK(r.col[3] == 0xAAAA0004u);
    // The same registers through MMIO.
    CHECK(rig.r32(0x10003C30u) == 0x1234u);
    CHECK(rig.r32(0x10003C70u) == 0x89ABCDEFu);
    CHECK(rig.r32(0x10003D00u) == 0x11111111u);
    CHECK(rig.r32(0x10003D70u) == 0xAAAA0004u);
}

TEST_CASE("mmio devices: VIF UNPACK V4-16 sign and zero extension follow immediate bit14", "[mmio][vif]") {
    EeRig rig;
    const uint16_t comps[4] = {0xFF80u, 0x0001u, 0x7FFFu, 0x8001u};
    std::vector<uint8_t> signPacket;
    put32(signPacket, makeVifCmd(0x6Du, 1u, 0x0000u)); // sign-extend
    for (uint16_t c : comps) {
        signPacket.insert(signPacket.end(), reinterpret_cast<const uint8_t*>(&c),
                          reinterpret_cast<const uint8_t*>(&c) + 2);
    }
    rig.vif1().push(signPacket);
    uint32_t v[4] = {};
    std::memcpy(v, rig.vu().data(1).data(), 16);
    CHECK(v[0] == 0xFFFFFF80u);
    CHECK(v[1] == 0x00000001u);
    CHECK(v[2] == 0x00007FFFu);
    CHECK(v[3] == 0xFFFF8001u);

    std::vector<uint8_t> zeroPacket;
    put32(zeroPacket, makeVifCmd(0x6Du, 1u, 0x4000u)); // zero-extend
    for (uint16_t c : comps) {
        zeroPacket.insert(zeroPacket.end(), reinterpret_cast<const uint8_t*>(&c),
                          reinterpret_cast<const uint8_t*>(&c) + 2);
    }
    rig.vif1().push(zeroPacket);
    std::memcpy(v, rig.vu().data(1).data(), 16);
    CHECK(v[0] == 0x0000FF80u);
    CHECK(v[1] == 0x00000001u);
    CHECK(v[2] == 0x00007FFFu);
    CHECK(v[3] == 0x00008001u);
}

TEST_CASE("mmio devices: VIF UNPACK bit15 adds TOPS to destination address", "[mmio][vif]") {
    EeRig rig;
    rig.w32(0x10003CC0u, 4u); // TOPS
    rig.vif1().push(words({makeVifCmd(0x6Cu, 1u, static_cast<uint16_t>(0x8000u | 0x0002u)), 0x11111111u,
                           0x22222222u, 0x33333333u, 0x44444444u}));
    const uint8_t* vu = rig.vu().data(1).data();
    uint32_t untouched = 0xDEADBEEFu;
    std::memcpy(&untouched, vu + 2u * 16u, 4);
    CHECK(untouched == 0u); // base addr without TOPS should remain untouched
    uint32_t q[4] = {};
    std::memcpy(q, vu + 6u * 16u, 16);
    CHECK(q[0] == 0x11111111u);
    CHECK(q[1] == 0x22222222u);
    CHECK(q[2] == 0x33333333u);
    CHECK(q[3] == 0x44444444u);
}

TEST_CASE("mmio devices: VIF STCYCL skip mode advances destination by CL when CL>=WL", "[mmio][vif]") {
    EeRig rig;
    rig.vif1().push(words({makeVifCmd(0x01u, 0u, static_cast<uint16_t>((1u << 8) | 3u)), // STCYCL WL=1 CL=3
                           makeVifCmd(0x6Cu, 2u, 0u), 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
                           0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu, 0xDDDDDDDDu}));
    const uint8_t* vu = rig.vu().data(1).data();
    uint32_t x[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        std::memcpy(&x[i], vu + i * 16u, 4);
    }
    CHECK(x[0] == 0x11111111u); // first vector should write at addr 0
    CHECK(x[1] == 0u);          // skip mode should leave addr 1 untouched when WL=1 CL=3
    CHECK(x[2] == 0u);          // skip mode should leave addr 2 untouched when WL=1 CL=3
    CHECK(x[3] == 0xAAAAAAAAu); // second vector should write at addr CL (addr 3)
}

TEST_CASE("mmio devices: VIF masked UNPACK uses data row col and protect selectors", "[mmio][vif]") {
    EeRig rig;
    const uint32_t preservedW = 0xDEADBEEFu;
    std::memcpy(rig.vu().data(1).data() + 12u, &preservedW, 4u);
    rig.vif1().push(words({makeVifCmd(0x20u, 0u, 0u), 0x000000E4u, // m0 data, m1 row, m2 col, m3 protect
                           makeVifCmd(0x30u, 0u, 0u), 0xAAAAB001u, 0xAAAAB002u, 0xAAAAB003u, 0xAAAAB004u,
                           makeVifCmd(0x31u, 0u, 0u), 0x11110001u, 0x11110002u, 0x11110003u, 0x11110004u,
                           makeVifCmd(0x7Cu, 1u, 0u), 0x01020304u, 0x11121314u, 0x21222324u, 0x31323334u}));
    uint32_t q[4] = {};
    std::memcpy(q, rig.vu().data(1).data(), 16);
    CHECK(q[0] == 0x01020304u); // mask=0 should write decompressed data
    CHECK(q[1] == 0xAAAAB002u); // mask=1 should write row register for Y field
    CHECK(q[2] == 0x11110001u); // mask=2 should write C0 on first write cycle
    CHECK(q[3] == preservedW);  // mask=3 should write-protect destination field
}

TEST_CASE("mmio devices: VIF STMOD offset and difference modes apply to UNPACK data", "[mmio][vif]") {
    EeRig rig;
    rig.vif1().push(words({makeVifCmd(0x30u, 0u, 0u), 10u,  20u,  30u,  40u,  // STROW
                           makeVifCmd(0x05u, 0u, 1u),                         // STMOD offset
                           makeVifCmd(0x6Cu, 1u, 0u), 1u,   2u,   3u,   4u,   // UNPACK V4-32 -> addr 0
                           makeVifCmd(0x30u, 0u, 0u), 100u, 100u, 100u, 100u, // STROW
                           makeVifCmd(0x05u, 0u, 2u),                         // STMOD difference
                           makeVifCmd(0x6Cu, 2u, 1u), 1u,   1u,   1u,   1u,   2u, 2u, 2u, 2u}));
    const uint8_t* vu = rig.vu().data(1).data();
    uint32_t q0[4] = {};
    std::memcpy(q0, vu, 16);
    CHECK(q0[0] == 11u);
    CHECK(q0[1] == 22u);
    CHECK(q0[2] == 33u);
    CHECK(q0[3] == 44u);
    uint32_t x1 = 0;
    uint32_t x2 = 0;
    std::memcpy(&x1, vu + 16u, 4u);
    std::memcpy(&x2, vu + 32u, 4u);
    CHECK(x1 == 101u); // difference mode first write should add initial row
    CHECK(x2 == 103u); // difference mode second write should accumulate updated row
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(rig.vif1().registers().row[i] == 103u); // difference mode should update the row registers
    }
}

TEST_CASE("mmio devices: VIF fill write uses STMASK and STROW when WL>CL", "[mmio][vif]") {
    EeRig rig;
    rig.vif1().push(words({makeVifCmd(0x01u, 0u, static_cast<uint16_t>((3u << 8) | 1u)), // STCYCL WL=3 CL=1
                           makeVifCmd(0x20u, 0u, 0u), 0x55555555u, // every field from row
                           makeVifCmd(0x30u, 0u, 0u), 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
                           makeVifCmd(0x7Cu, 3u, 0u), 0xAAAABBBBu, 0xCCCCDDDDu, 0xEEEEFFFFu, 0x12345678u}));
    const uint8_t* vu = rig.vu().data(1).data();
    for (uint32_t i = 0; i < 3u; ++i) {
        uint32_t q[4] = {};
        std::memcpy(q, vu + i * 16u, 16);
        CHECK(q[0] == 0x11111111u);
        CHECK(q[1] == 0x22222222u);
        CHECK(q[2] == 0x33333333u);
        CHECK(q[3] == 0x44444444u);
    }
    // One input vector was consumed for CL=1, WL=3: the VIF waits for the next command.
    CHECK((rig.r32(0x10003C00u) & Vif::stat_vps_waiting) == 0u);
}

TEST_CASE("mmio devices: VIF irq command sets STAT.INT and CODE until FBRST.STC clears it", "[mmio][vif]") {
    EeRig rig;
    const uint32_t irqMarkCmd = 0x80000000u | makeVifCmd(0x07u, 0x12u, 0x3456u);
    rig.vif1().push(words({irqMarkCmd}));
    CHECK(rig.vif1().registers().code == irqMarkCmd); // VIF CODE should capture the last command
    CHECK((rig.r32(0x10003C00u) & (1u << 11)) != 0u); // irq bit should raise VIF1 STAT.INT
    CHECK(rig.vif1().registers().mark == 0x3456u);    // MARK command should still update MARK
    CHECK((rig.machine.interrupts().intc_stat() & (1u << static_cast<unsigned>(IntcLine::vif1))) != 0u);
    rig.w32(0x10003C10u, 0x8u);                       // FBRST.STC
    CHECK((rig.r32(0x10003C00u) & (1u << 11)) == 0u); // FBRST.STC should clear VIF1 STAT.INT
}

TEST_CASE("mmio devices: VIF FBRST RST clears VIF1 command state", "[mmio][vif]") {
    EeRig rig;
    // Upstream poked the registers; here the VIF reaches that state through VIFcodes and register writes.
    rig.vif1().push(words({makeVifCmd(0x07u, 0u, 0x1234u), makeVifCmd(0x01u, 0u, 0x0102u),
                           makeVifCmd(0x05u, 0u, 2u), makeVifCmd(0x20u, 7u, 0u), 0x89ABCDEFu}));
    REQUIRE(rig.vif1().registers().mark == 0x1234u);
    rig.w32(0x10003C10u, 0x1u); // FBRST.RST
    const Vif::Registers& r = rig.vif1().registers();
    CHECK(r.mark == 0u);
    CHECK(r.cycle == 0u);
    CHECK(r.mode == 0u);
    CHECK(r.num == 0u);
    CHECK(r.mask == 0u);
    CHECK(r.code == 0u);
    CHECK(r.stat == 0u);
}

TEST_CASE("mmio devices: VIF double-buffer OFFSET BASE and MSCAL update TOPS and ITOPS", "[mmio][vif]") {
    EeRig rig;
    // A micro program at 0x18 that copies TOP and ITOP into VI2/VI3 stands in for upstream's MSCAL callback;
    // a second one at 0x28 ends the MSCNT that follows.
    constexpr uint32_t ebit = 1u << 30;
    constexpr uint32_t nop = 0x000002FFu;
    auto pair = [&](uint32_t pc, uint32_t lower, uint32_t upper) {
        std::memcpy(rig.vu().micro(1).data() + pc, &lower, 4);
        std::memcpy(rig.vu().micro(1).data() + pc + 4, &upper, 4);
    };
    pair(0x18u, (0x40u << 25) | (2u << 16) | (0x68u & 0x7Cu) << 4 | (0x68u & 3u) | 0x3Cu,
         nop | ebit);                                                                           // XTOP vi2
    pair(0x20u, (0x40u << 25) | (3u << 16) | (0x69u & 0x7Cu) << 4 | (0x69u & 3u) | 0x3Cu, nop); // XITOP vi3
    pair(0x28u, 0u, nop | ebit);
    rig.vu().touch_micro(1);

    rig.w32(0x10003CA0u, 0x120u);                             // BASE
    rig.w32(0x10003CC0u, 0x120u);                             // TOPS
    rig.vif1().push(words({makeVifCmd(0x02u, 0u, 0x0022u)})); // OFFSET
    CHECK(rig.vif1().registers().ofst == 0x22u);
    CHECK(rig.vif1().registers().base == 0x120u);
    CHECK((rig.vif1().registers().stat & Vif::stat_dbf) == 0u); // OFFSET should clear DBF
    CHECK(rig.vif1().registers().tops == 0x120u);               // DBF=0 keeps TOPS at BASE

    rig.vif1().push(words({makeVifCmd(0x03u, 0u, 0x0030u)})); // BASE
    CHECK(rig.vif1().registers().base == 0x30u);
    CHECK(rig.vif1().registers().tops == 0x120u); // BASE should not rewrite current TOPS

    rig.vif1().push(words({makeVifCmd(0x04u, 0u, 0x0044u)})); // ITOP
    CHECK(rig.vif1().registers().itops == 0x44u);

    rig.vif1().push(words({makeVifCmd(0x14u, 0u, 0x0003u)})); // MSCAL 0x18
    const VuState& vu = rig.vu().interpreter(1).state();
    CHECK(vu.vi[2] == 0x120);                                   // the program should see TOP = TOPS
    CHECK(vu.vi[3] == 0x44);                                    // and ITOP = ITOPS
    CHECK(rig.vif1().registers().top == 0x120u);                // MSCAL should latch TOP from TOPS
    CHECK(rig.vif1().registers().itop == 0x44u);                // MSCAL should latch ITOP from ITOPS
    CHECK((rig.vif1().registers().stat & Vif::stat_dbf) != 0u); // MSCAL should toggle DBF
    CHECK(rig.vif1().registers().tops == 0x52u);                // DBF=1 should set TOPS to BASE+OFST

    rig.vif1().push(words({makeVifCmd(0x17u, 0u, 0u)}));        // MSCNT
    CHECK((rig.vif1().registers().stat & Vif::stat_dbf) == 0u); // MSCNT should toggle DBF again
    CHECK(rig.vif1().registers().tops == 0x30u);                // DBF=0 should restore TOPS to BASE
}

TEST_CASE("mmio devices: VIF MSKPATH3 uses immediate bit15", "[mmio][vif][gif]") {
    EeRig rig;
    rig.vif1().push(words({makeVifCmd(0x06u, 0u, 0x8000u)}));
    CHECK((rig.r32(kGifStat) & Gif::stat_m3p) != 0u); // MSKPATH3 with bit15 set should enable the mask
    rig.vif1().push(words({makeVifCmd(0x06u, 0u, 0x0000u)}));
    CHECK((rig.r32(kGifStat) & Gif::stat_m3p) == 0u); // bit15 clear should disable it
}

TEST_CASE("mmio devices: VIF1 FIFO MSKPATH3 is visible through GIF_STAT", "[mmio][vif][gif]") {
    EeRig rig;
    uint8_t q[16] = {};
    const uint32_t setMask = makeVifCmd(0x06u, 0u, 0x8000u);
    std::memcpy(q, &setMask, 4);
    rig.w128(0x10005000u, q);
    CHECK((rig.r32(kGifStat) & 0x2u) != 0u); // GIF_STAT.M3P should report the VIF1 PATH3 mask
    const uint32_t clearMask = makeVifCmd(0x06u, 0u, 0x0000u);
    std::memcpy(q, &clearMask, 4);
    rig.w128(0x10005000u, q);
    CHECK((rig.r32(kGifStat) & 0x2u) == 0u); // GIF_STAT.M3P should clear with the VIF1 PATH3 mask

    rig.w32(0x10003010u, 0x5u);                // GIF_MODE: M3R | IMT
    rig.w32(0x10003000u, 0x8u);                // GIF_CTRL: PSE
    CHECK((rig.r32(kGifStat) & 0xDu) == 0xDu); // GIF_STAT should mirror GIF_MODE and GIF_CTRL status bits
}

TEST_CASE("mmio devices: GIF_STAT reports an idle GIF after a synchronous DMA", "[mmio][gif][dmac]") {
    // Upstream kept FQC = 1 visible for one quantum after a synchronous transfer. Here the data has gone to
    // the GS when the DMA starts, so FQC reads 0; what the guest can watch is the channel's STR bit.
    EeRig rig;
    constexpr uint32_t kSource = 0x00020000u;
    writeEopQword(rig.mem().rdram(), kSource, 0x10u);
    rig.w32(kGifCh + 0x10u, kSource);
    rig.w32(kGifCh + 0x20u, 1u);
    rig.w32(kGifCh + 0x00u, 0x100u);
    CHECK(((rig.r32(kGifStat) >> 24u) & 0x1Fu) == 0u);
    CHECK((rig.r32(kGifCh) & 0x100u) != 0u); // busy until the completion event
    drain(rig);
    CHECK((rig.r32(kGifCh) & 0x100u) == 0u);
}

TEST_CASE("mmio devices: PATH3 mask queues packets until unmask", "[mmio][gif]") {
    EeRig rig;
    std::vector<uint8_t> packetA(16u);
    std::vector<uint8_t> packetB(16u);
    writeEopQword(packetA.data(), 0u, 0x10u);
    writeEopQword(packetB.data(), 0u, 0x40u);
    rig.vif1().push(words({makeVifCmd(0x06u, 0u, 0x8000u)}));
    rig.gif().path_data(dq8::gs::GifPath::path3, packetA);
    rig.gif().path_data(dq8::gs::GifPath::path3, packetB);
    CHECK(rig.sink.transfers.empty()); // masked PATH3 packets should be queued, not dropped/emitted
    CHECK((rig.r32(kGifStat) & Gif::stat_p3q) != 0u);
    rig.vif1().push(words({makeVifCmd(0x06u, 0u, 0x0000u)}));
    REQUIRE(rig.sink.transfers.size() == 2u);              // unmask should flush queued PATH3 packets
    CHECK(isEopQword(rig.sink.transfers[0].bytes, 0x10u)); // in order
    CHECK(isEopQword(rig.sink.transfers[1].bytes, 0x40u));
}

TEST_CASE("mmio devices: packets from different paths reach the GS in completion order", "[mmio][gif]") {
    // Upstream's arbiter drained queued PATH1, PATH2, PATH3 packets by priority. Every path here delivers a
    // packet the moment it completes, so the GS sees them in the order the paths finished them.
    EeRig rig;
    std::vector<uint8_t> p1(16u);
    std::vector<uint8_t> p2(16u);
    std::vector<uint8_t> p3(16u);
    writeEopQword(p1.data(), 0u, 0x11u);
    writeEopQword(p2.data(), 0u, 0x22u);
    writeEopQword(p3.data(), 0u, 0x33u);
    rig.gif().path_data(dq8::gs::GifPath::path3, p3);
    rig.gif().path_data(dq8::gs::GifPath::path2, p2);
    rig.gif().path_data(dq8::gs::GifPath::path1, p1);
    REQUIRE(rig.sink.transfers.size() == 3u);
    CHECK(rig.sink.transfers[0].path == dq8::gs::GifPath::path3);
    CHECK(rig.sink.transfers[1].path == dq8::gs::GifPath::path2);
    CHECK(rig.sink.transfers[2].path == dq8::gs::GifPath::path1);
}

TEST_CASE("mmio devices: VIF DIRECTHL stalls behind queued PATH3 IMAGE packets", "[mmio][vif][gif]") {
    EeRig rig;
    std::vector<uint8_t> path3Image;
    put64(path3Image, makeGifTag(0u, GIF_FMT_IMAGE, 0u, true)); // IMAGE packet marker
    put64(path3Image, 0xAAull);
    rig.gif().path_data(dq8::gs::GifPath::path3, path3Image);

    std::vector<uint8_t> vifPacket;
    put32(vifPacket, makeVifCmd(0x51u, 0u, 1u)); // DIRECTHL 1 QW
    put64(vifPacket, makeGifTag(0u, GIF_FMT_PACKED, 1u, true));
    put64(vifPacket, 0xD2ull);
    rig.vif1().push(vifPacket);

    REQUIRE(rig.sink.transfers.size() == 2u);       // PATH3 and DIRECTHL packets should both drain
    CHECK(rig.sink.transfers[0].bytes[8] == 0xAAu); // DIRECTHL should not preempt the PATH3 IMAGE packet
    CHECK(rig.sink.transfers[1].bytes[8] == 0xD2u);
    CHECK(rig.sink.transfers[1].path == dq8::gs::GifPath::path2);
}

TEST_CASE("mmio devices: GIF DMA mode0 copies RDRAM packet and clears channel", "[mmio][dmac][gif]") {
    EeRig rig;
    constexpr uint32_t kSrc = 0x00022000u;
    constexpr uint32_t kQwc = 2u;
    writeImagePacket(rig.mem().rdram(), kSrc, 1u, 0x40u);
    rig.w32(kGifCh + 0x10u, kSrc);
    rig.w32(kGifCh + 0x20u, kQwc);
    rig.w32(kGifCh + 0x00u, 0x100u);
    CHECK(rig.dmac().stats(DmaChannel::gif).transfers == 1u); // starting GIF DMA should count a transfer
    drain(rig);
    REQUIRE(rig.sink.transfers.size() == 1u);                // GIF DMA should emit one packet
    CHECK(rig.sink.transfers[0].bytes.size() == kQwc * 16u); // GIF packet size should match QWC
    CHECK(std::memcmp(rig.sink.transfers[0].bytes.data(), rig.mem().rdram() + kSrc, kQwc * 16u) == 0);
    CHECK((rig.r32(kGifCh + 0x00u) & 0x100u) == 0u); // GIF CHCR STR bit should be cleared after drain
    CHECK(rig.r32(kGifCh + 0x20u) == 0u);            // GIF QWC should be cleared after drain
}

TEST_CASE("mmio devices: GIF DMA can source from scratchpad", "[mmio][dmac][gif]") {
    EeRig rig;
    constexpr uint32_t kSrcScratch = 0x70000000u + 0x80u;
    writeEopQword(rig.mem().scratchpad(), 0x80u, 0xA0u);
    rig.w32(kGifCh + 0x10u, kSrcScratch);
    rig.w32(kGifCh + 0x20u, 1u);
    rig.w32(kGifCh + 0x00u, 0x100u);
    REQUIRE(rig.sink.transfers.size() == 1u);
    CHECK(isEopQword(rig.sink.transfers[0].bytes, 0xA0u)); // packet bytes should match scratchpad source
}

TEST_CASE("mmio devices: GIF DMA chain can source tags and payload from 0xF000 scratchpad alias",
          "[mmio][dmac][gif]") {
    // As a DMA address, 0xF0000100 has bit 31 (SPR) set: tags and data come from the scratchpad.
    EeRig rig;
    uint8_t* scratch = rig.mem().scratchpad();
    writeDmaTag(scratch, 0x100u, makeDmaTag(1u, 7u, 0u, false));
    writeEopQword(scratch, 0x110u, 0xC0u);
    rig.w32(kGifCh + 0x30u, 0xF0000100u);
    rig.w32(kGifCh + 0x00u, 0x104u);
    REQUIRE(rig.sink.transfers.size() == 1u);
    CHECK(isEopQword(rig.sink.transfers[0].bytes, 0xC0u));
}

TEST_CASE("mmio devices: canonical load-image chain completes the GIF channel", "[mmio][dmac][gif]") {
    // Upstream: "native GIF image upload recognizes canonical load-image chain". Without a GS model the
    // check is the DMAC and GIF side: the setup and IMAGE packets reach the GS, D_STAT and CHCR settle.
    EeRig rig;
    constexpr uint32_t kChain = 0x00028000u;
    constexpr uint32_t kPixels = 0x00029000u;
    uint8_t* rdram = rig.mem().rdram();
    for (uint32_t i = 0; i < 16u; ++i) {
        rdram[kPixels + i] = static_cast<uint8_t>(0x40u + i);
    }
    uint32_t a = kChain;
    writeDmaTag(rdram, a, makeDmaTag(5u, 1u, 0u)); // CNT: setup tag + four A+D writes
    a += 16u;
    writeU64(rdram, a, makeGifTag(4u, GIF_FMT_PACKED, 1u, false));
    writeU64(rdram, a + 8u, 0x0Eull);
    a += 16u;
    const uint64_t regs[4][2] = {{1ull << 48, GS_REG_BITBLTBUF},
                                 {0, GS_REG_TRXPOS},
                                 {4ull | (1ull << 32), GS_REG_TRXREG},
                                 {0, GS_REG_TRXDIR}};
    for (const auto& r : regs) {
        writeU64(rdram, a, r[0]);
        writeU64(rdram, a + 8u, r[1]);
        a += 16u;
    }
    writeDmaTag(rdram, a, makeDmaTag(1u, 1u, 0u)); // CNT: IMAGE tag
    a += 16u;
    writeU64(rdram, a, makeGifTag(1u, GIF_FMT_IMAGE, 0u, true));
    writeU64(rdram, a + 8u, 0u);
    a += 16u;
    writeDmaTag(rdram, a, makeDmaTag(1u, 3u, kPixels)); // REF: pixels
    a += 16u;
    writeDmaTag(rdram, a, makeDmaTag(0u, 7u, 0u)); // END

    rig.w32(kGifCh + 0x30u, kChain);
    rig.w32(kGifCh + 0x00u, 0x105u);
    drain(rig);
    REQUIRE(rig.sink.transfers.size() == 1u); // the setup and the IMAGE data form one packet (EOP on IMAGE)
    CHECK(rig.sink.transfers[0].bytes.size() == (1u + 4u + 1u + 1u) * 16u);
    CHECK(std::memcmp(rig.sink.transfers[0].bytes.data() + 6u * 16u, rdram + kPixels, 16u) == 0);
    CHECK((rig.r32(kDStat) & (1u << 2u)) != 0u);                   // GIF completion should raise D_STAT bit 2
    CHECK(rig.r32(kGifCh + 0x20u) == 0u);                          // QWC cleared
    CHECK((rig.r32(kGifCh + 0x00u) & 0x100u) == 0u);               // STR cleared
    CHECK((rig.r32(kGifCh + 0x00u) & 0x70000000u) == 0x70000000u); // the terminal END tag id is latched
}

TEST_CASE("mmio devices: GIF DMA chain REF keeps CT32 image data after paletted upload",
          "[mmio][dmac][gif]") {
    // Two texture uploads through REF tags in one chain: the second packet must carry the CT32 bytes, not
    // the T4 payload of the first (upstream checked GS VRAM; the packets carry the same evidence).
    EeRig rig;
    constexpr uint32_t kChain = 0x00028000u;
    constexpr uint32_t kT4Data = 0x00029000u;
    constexpr uint32_t kCt32Data = 0x0002A000u;
    constexpr uint32_t kT4Bytes = 16u * 16u / 2u;
    constexpr uint32_t kCt32Bytes = 16u * 16u * 4u;
    uint8_t* rdram = rig.mem().rdram();
    for (uint32_t i = 0; i < kT4Bytes; ++i) {
        rdram[kT4Data + i] = static_cast<uint8_t>(0x10u + (i % 7u));
    }
    for (uint32_t i = 0; i < kCt32Bytes; ++i) {
        rdram[kCt32Data + i] = static_cast<uint8_t>(i * 13u);
    }
    uint32_t a = kChain;
    auto upload = [&](uint32_t dpsm, uint32_t data, uint32_t bytes) {
        writeDmaTag(rdram, a, makeDmaTag(5u, 1u, 0u));
        a += 16u;
        writeU64(rdram, a, makeGifTag(4u, GIF_FMT_PACKED, 1u, false));
        writeU64(rdram, a + 8u, 0x0Eull);
        a += 16u;
        const uint64_t regs[4][2] = {{(1ull << 48) | (static_cast<uint64_t>(dpsm) << 56), GS_REG_BITBLTBUF},
                                     {0, GS_REG_TRXPOS},
                                     {16ull | (16ull << 32), GS_REG_TRXREG},
                                     {0, GS_REG_TRXDIR}};
        for (const auto& r : regs) {
            writeU64(rdram, a, r[0]);
            writeU64(rdram, a + 8u, r[1]);
            a += 16u;
        }
        writeDmaTag(rdram, a, makeDmaTag(1u, 1u, 0u));
        a += 16u;
        writeU64(rdram, a, makeGifTag(static_cast<uint16_t>(bytes / 16u), GIF_FMT_IMAGE, 0u, true));
        writeU64(rdram, a + 8u, 0u);
        a += 16u;
        writeDmaTag(rdram, a, makeDmaTag(static_cast<uint16_t>(bytes / 16u), 3u, data));
        a += 16u;
    };
    upload(0x24u, kT4Data, kT4Bytes); // PSMT4HL
    upload(0x00u, kCt32Data, kCt32Bytes);
    writeDmaTag(rdram, a, makeDmaTag(0u, 7u, 0u));
    rig.w32(kGifCh + 0x30u, kChain);
    rig.w32(kGifCh + 0x00u, 0x104u);
    REQUIRE(rig.sink.transfers.size() == 2u);
    const auto& ct32 = rig.sink.transfers[1].bytes;
    REQUIRE(ct32.size() == 6u * 16u + kCt32Bytes);
    CHECK(std::memcmp(ct32.data() + 6u * 16u, rdram + kCt32Data, kCt32Bytes) == 0);
    CHECK(std::memcmp(rig.sink.transfers[0].bytes.data() + 6u * 16u, rdram + kT4Data, kT4Bytes) == 0);
}

TEST_CASE("mmio devices: VIF1 DMA DIRECT forwards payload to GIF callback and clears channel",
          "[mmio][dmac][vif]") {
    EeRig rig;
    constexpr uint32_t kSrc = 0x00024000u;
    constexpr uint32_t kQwc = 2u;
    uint8_t* rdram = rig.mem().rdram();
    std::memset(rdram + kSrc, 0, kQwc * 16u);
    const uint32_t cmd = makeVifCmd(0x50u, 0u, 1u); // DIRECT 1 QW
    std::memcpy(rdram + kSrc, &cmd, sizeof(cmd));
    writeEopQword(rdram, kSrc + 4u, 0x11u);
    rig.w32(kVif1Ch + 0x10u, kSrc);
    rig.w32(kVif1Ch + 0x20u, kQwc);
    rig.w32(kVif1Ch + 0x00u, 0x101u); // STR | DIR (from memory)
    drain(rig);
    REQUIRE(rig.sink.transfers.size() == 1u);
    CHECK(rig.sink.transfers[0].path == dq8::gs::GifPath::path2);
    CHECK(isEopQword(rig.sink.transfers[0].bytes, 0x11u));
    CHECK((rig.r32(kVif1Ch + 0x00u) & 0x100u) == 0u);
    CHECK(rig.r32(kVif1Ch + 0x20u) == 0u);
}

TEST_CASE("mmio devices: VIF1 DMA chain preserves compact tag high bytes for DIRECT packets",
          "[mmio][dmac][vif]") {
    EeRig rig;
    constexpr uint32_t kTag = 0x00025000u;
    uint8_t* rdram = rig.mem().rdram();
    writeDmaTag(rdram, kTag, makeDmaTag(1u, 7u, 0u));
    const uint32_t directCmd = makeVifCmd(0x50u, 0u, 1u);
    std::memcpy(rdram + kTag + 12u, &directCmd, sizeof(directCmd)); // DIRECT in the tag's upper half
    writeEopQword(rdram, kTag + 16u, 0x70u);
    rig.w32(kVif1Ch + 0x30u, kTag);
    rig.w32(kVif1Ch + 0x00u, 0x145u); // STR | CHAIN | TTE | DIR
    drain(rig);
    REQUIRE(rig.sink.transfers.size() == 1u);
    CHECK(isEopQword(rig.sink.transfers[0].bytes, 0x70u));
    CHECK((rig.r32(kVif1Ch + 0x00u) & 0x100u) == 0u);
}

TEST_CASE("mmio devices: VIF1 DMA chain preserves compact tag high bytes when qwc is zero",
          "[mmio][dmac][vif]") {
    EeRig rig;
    constexpr uint32_t kTag = 0x00025100u;
    uint8_t* rdram = rig.mem().rdram();
    writeDmaTag(rdram, kTag, makeDmaTag(0u, 7u, 0u));
    const uint32_t itopCmd = makeVifCmd(0x04u, 0u, 0x44u);
    std::memcpy(rdram + kTag + 12u, &itopCmd, sizeof(itopCmd));
    rig.w32(kVif1Ch + 0x30u, kTag);
    rig.w32(kVif1Ch + 0x00u, 0x145u);
    drain(rig);
    CHECK(rig.vif1().registers().itops == 0x44u);
    CHECK((rig.r32(kVif1Ch + 0x00u) & 0x100u) == 0u);
}

TEST_CASE("mmio devices: VIF1 DMA chain transfers REF tag high bytes when TTE is enabled",
          "[mmio][dmac][vif]") {
    EeRig rig;
    constexpr uint32_t kTag0 = 0x00025200u;
    constexpr uint32_t kTag1 = kTag0 + 0x10u;
    constexpr uint32_t kRefPayload = 0x00025300u;
    uint8_t* rdram = rig.mem().rdram();
    writeDmaTag(rdram, kTag0, makeDmaTag(1u, 3u, kRefPayload)); // REF
    writeDmaTag(rdram, kTag1, makeDmaTag(0u, 7u, 0u));          // END
    const uint32_t directCmd = makeVifCmd(0x50u, 0u, 1u);
    std::memcpy(rdram + kTag0 + 12u, &directCmd, sizeof(directCmd));
    writeEopQword(rdram, kRefPayload, 0xA0u);
    rig.w32(kVif1Ch + 0x30u, kTag0);
    rig.w32(kVif1Ch + 0x00u, 0x145u);
    REQUIRE(rig.sink.transfers.size() == 1u);
    CHECK(isEopQword(rig.sink.transfers[0].bytes, 0xA0u)); // REF payload reaches the GS without desync
}

TEST_CASE("mmio devices: VIF1 DMA chain ignores tag high bytes when TTE is disabled", "[mmio][dmac][vif]") {
    EeRig rig;
    constexpr uint32_t kTag = 0x00025400u;
    uint8_t* rdram = rig.mem().rdram();
    writeDmaTag(rdram, kTag, makeDmaTag(0u, 7u, 0u));
    const uint32_t itopCmd = makeVifCmd(0x04u, 0u, 0x55u);
    std::memcpy(rdram + kTag + 12u, &itopCmd, sizeof(itopCmd));
    rig.w32(kVif1Ch + 0x30u, kTag);
    rig.w32(kVif1Ch + 0x00u, 0x105u); // no TTE
    drain(rig);
    CHECK(rig.vif1().registers().itops == 0u);
}

TEST_CASE("mmio devices: VIF1 DMA chain latches terminal tag bits in CHCR", "[mmio][dmac][vif]") {
    EeRig rig;
    constexpr uint32_t kTag0 = 0x00027400u;
    constexpr uint32_t kTag1 = kTag0 + 0x20u;
    uint8_t* rdram = rig.mem().rdram();
    writeDmaTag(rdram, kTag0, makeDmaTag(1u, 1u, 0u)); // CNT with one NOP quadword
    std::memset(rdram + kTag0 + 0x10u, 0, 0x10u);
    writeDmaTag(rdram, kTag1, makeDmaTag(0u, 7u, 0u)); // END
    rig.w32(kVif1Ch + 0x30u, kTag0);
    rig.w32(kVif1Ch + 0x00u, 0x185u);
    drain(rig);
    const uint32_t chcr = rig.r32(kVif1Ch + 0x00u);
    CHECK((chcr & 0x100u) == 0u);
    CHECK((chcr & 0x70000000u) == 0x70000000u);
    CHECK((rig.r32(kDStat) & 0x2u) != 0u);
}

TEST_CASE("mmio devices: GIF DMA chain CALL sources payload from TADR+16", "[mmio][dmac][gif]") {
    EeRig rig;
    constexpr uint32_t kTag0 = 0x00026000u;
    constexpr uint32_t kTag1 = 0x00026100u;
    uint8_t* rdram = rig.mem().rdram();
    writeDmaTag(rdram, kTag0, makeDmaTag(1u, 5u, kTag1)); // CALL qwc=1 addr=kTag1
    writeDmaTag(rdram, kTag1, makeDmaTag(1u, 7u, 0u));    // END qwc=1
    // CALL payload: an IMAGE tag for one quadword; END payload: that quadword.
    writeU64(rdram, kTag0 + 16u, makeGifTag(1u, GIF_FMT_IMAGE, 0u, true));
    writeU64(rdram, kTag0 + 24u, 0x4746454443424140ull);
    for (uint32_t i = 0; i < 16u; ++i) {
        rdram[kTag1 + 16u + i] = static_cast<uint8_t>(0x80u + i);
    }
    rig.w32(kGifCh + 0x30u, kTag0);
    rig.w32(kGifCh + 0x00u, 0x104u);
    REQUIRE(rig.sink.transfers.size() == 1u);
    const auto& p = rig.sink.transfers[0].bytes;
    REQUIRE(p.size() == 32u);                                          // CALL+END should emit two qwords
    CHECK(std::memcmp(p.data(), rdram + kTag0 + 16u, 16u) == 0);       // CALL must transfer from TADR+16
    CHECK(std::memcmp(p.data() + 16u, rdram + kTag1 + 16u, 16u) == 0); // END payload follows
}

TEST_CASE("mmio devices: GIF DMA chain RET transfers payload and resumes after CALL", "[mmio][dmac][gif]") {
    EeRig rig;
    constexpr uint32_t kTagCall = 0x00026200u;
    constexpr uint32_t kTagRet = 0x00026300u;
    constexpr uint32_t kTagEnd = 0x00026220u;
    uint8_t* rdram = rig.mem().rdram();
    writeDmaTag(rdram, kTagCall, makeDmaTag(1u, 5u, kTagRet));
    writeDmaTag(rdram, kTagRet, makeDmaTag(1u, 6u, 0u));
    writeDmaTag(rdram, kTagEnd, makeDmaTag(1u, 7u, 0u));
    writeU64(rdram, kTagCall + 16u, makeGifTag(2u, GIF_FMT_IMAGE, 0u, true));
    writeU64(rdram, kTagCall + 24u, 0x1111111111111111ull);
    for (uint32_t i = 0; i < 16u; ++i) {
        rdram[kTagRet + 16u + i] = static_cast<uint8_t>(0x22u + i);
        rdram[kTagEnd + 16u + i] = static_cast<uint8_t>(0x33u + i);
    }
    rig.w32(kGifCh + 0x30u, kTagCall);
    rig.w32(kGifCh + 0x00u, 0x104u);
    REQUIRE(rig.sink.transfers.size() == 1u);
    const auto& p = rig.sink.transfers[0].bytes;
    REQUIRE(p.size() == 48u);                                            // CALL+RET+END
    CHECK(std::memcmp(p.data(), rdram + kTagCall + 16u, 16u) == 0);      // CALL payload first
    CHECK(std::memcmp(p.data() + 16u, rdram + kTagRet + 16u, 16u) == 0); // RET transfers its own payload
    CHECK(std::memcmp(p.data() + 32u, rdram + kTagEnd + 16u, 16u) == 0); // and resumes after CALL
}

TEST_CASE("mmio devices: GIF DMA chain IRQ stops only when TIE is set", "[mmio][dmac][gif]") {
    constexpr uint32_t kTag0 = 0x00026400u;
    constexpr uint32_t kTag1 = 0x00026410u;
    constexpr uint32_t kRefData = 0x00026500u;
    auto runChain = [&](uint32_t chcrValue) {
        EeRig rig;
        uint8_t* rdram = rig.mem().rdram();
        writeDmaTag(rdram, kTag0, makeDmaTag(1u, 3u, kRefData, true)); // REF + IRQ
        writeDmaTag(rdram, kTag1, makeDmaTag(1u, 7u, 0u));             // END
        writeEopQword(rdram, kRefData, 0x55u);
        writeEopQword(rdram, kTag1 + 16u, 0x77u);
        rig.w32(kGifCh + 0x30u, kTag0);
        rig.w32(kGifCh + 0x00u, chcrValue);
        return rig.sink.transfers.size();
    };
    CHECK(runChain(0x104u) == 2u); // IRQ tag should not stop chain when TIE is clear
    CHECK(runChain(0x184u) == 1u); // IRQ tag should stop chain when TIE is set
}

TEST_CASE("mmio devices: DMAC D_STAT toggles masks and clears channel status on write-one", "[mmio][dmac]") {
    EeRig rig;
    constexpr uint32_t kGifMaskBit = 1u << 18;
    constexpr uint32_t kGifStatusBit = 1u << 2;
    rig.w32(kDStat, kGifMaskBit);
    CHECK((rig.r32(kDStat) & kGifMaskBit) != 0u); // first mask write should enable GIF mask bit
    rig.w32(kDStat, kGifMaskBit);
    CHECK((rig.r32(kDStat) & kGifMaskBit) == 0u); // second mask write should disable it
    rig.w32(kDStat, kGifMaskBit);

    constexpr uint32_t kSrc = 0x00027000u;
    writeEopQword(rig.mem().rdram(), kSrc, 0x90u);
    rig.w32(kGifCh + 0x10u, kSrc);
    rig.w32(kGifCh + 0x20u, 1u);
    rig.w32(kGifCh + 0x00u, 0x100u);
    CHECK((rig.r32(kDStat) & kGifStatusBit) == 0u); // status should not set before the completion event
    drain(rig);
    CHECK((rig.r32(kDStat) & kGifStatusBit) != 0u); // completion sets the channel status bit
    CHECK(rig.machine.interrupts().int1_pending()); // status & mask: INT1 (upstream's summary bit)
    rig.w32(kDStat, kGifStatusBit);
    CHECK((rig.r32(kDStat) & kGifStatusBit) == 0u); // write-one should clear GIF channel status bit
    CHECK_FALSE(rig.machine.interrupts().int1_pending());
}

TEST_CASE("mmio devices: DMAC D_CTRL DMAE gates GIF DMA start", "[mmio][dmac]") {
    EeRig rig;
    constexpr uint32_t kSrc = 0x00027800u;
    writeEopQword(rig.mem().rdram(), kSrc, 0xE0u);
    rig.w32(kDctrl, 0u);
    rig.w32(kGifCh + 0x10u, kSrc);
    rig.w32(kGifCh + 0x20u, 1u);
    rig.w32(kGifCh + 0x00u, 0x100u);
    drain(rig);
    CHECK(rig.sink.transfers.empty()); // DMAE=0 should prevent the transfer
    CHECK(rig.dmac().stats(DmaChannel::gif).transfers == 0u);
    rig.w32(kDctrl, 1u); // the waiting channel starts when DMAE is set
    REQUIRE(rig.sink.transfers.size() == 1u);
    CHECK(rig.sink.transfers[0].bytes.size() == 16u);
}

TEST_CASE("mmio devices: DMAC SPR_FROM copies scratchpad to RDRAM and completes channel 8", "[mmio][dmac]") {
    EeRig rig;
    constexpr uint32_t kChannel = 0x1000D000u;
    constexpr uint32_t kMadr = 0x00028000u;
    constexpr uint32_t kSadr = 0x00000120u;
    constexpr uint32_t kBytes = 32u;
    for (uint32_t i = 0; i < kBytes; ++i) {
        rig.mem().scratchpad()[kSadr + i] = static_cast<uint8_t>(0x30u + i);
    }
    rig.w32(kChannel + 0x10u, kMadr);
    rig.w32(kChannel + 0x20u, 2u);
    rig.w32(kChannel + 0x80u, kSadr);
    rig.w32(kChannel + 0x00u, 0x100u);
    bool copied = true;
    for (uint32_t i = 0; i < kBytes; ++i) {
        copied = copied && rig.mem().rdram()[kMadr + i] == static_cast<uint8_t>(0x30u + i);
    }
    CHECK(copied);
    drain(rig);
    CHECK((rig.r32(kChannel) & 0x100u) == 0u);
    CHECK(rig.r32(kChannel + 0x20u) == 0u);
    CHECK(rig.r32(kChannel + 0x10u) == kMadr + kBytes);
    CHECK(rig.r32(kChannel + 0x80u) == ((kSadr + kBytes) & 0x3FFFu));
    CHECK((rig.r32(kDStat) & (1u << 8u)) != 0u);
}

TEST_CASE("mmio devices: DMAC SPR_TO copies RDRAM to scratchpad and completes channel 9", "[mmio][dmac]") {
    EeRig rig;
    constexpr uint32_t kChannel = 0x1000D400u;
    constexpr uint32_t kMadr = 0x00028400u;
    constexpr uint32_t kSadr = 0x00000240u;
    constexpr uint32_t kBytes = 32u;
    for (uint32_t i = 0; i < kBytes; ++i) {
        rig.mem().rdram()[kMadr + i] = static_cast<uint8_t>(0x70u + i);
    }
    rig.w32(kChannel + 0x10u, kMadr);
    rig.w32(kChannel + 0x20u, 2u);
    rig.w32(kChannel + 0x80u, kSadr);
    rig.w32(kChannel + 0x00u, 0x100u);
    bool copied = true;
    for (uint32_t i = 0; i < kBytes; ++i) {
        copied = copied && rig.mem().scratchpad()[kSadr + i] == static_cast<uint8_t>(0x70u + i);
    }
    CHECK(copied);
    drain(rig);
    CHECK((rig.r32(kChannel) & 0x100u) == 0u);
    CHECK(rig.r32(kChannel + 0x20u) == 0u);
    CHECK((rig.r32(kDStat) & (1u << 9u)) != 0u);
}

TEST_CASE("mmio devices: VIF1 DMA DIRECT image packet reaches GS through PATH2", "[mmio][dmac][vif][gif]") {
    EeRig rig;
    constexpr uint32_t kSrc = 0x00027C00u;
    constexpr uint32_t kQwc = 3u;
    uint8_t* rdram = rig.mem().rdram();
    std::memset(rdram + kSrc, 0, kQwc * 16u);
    const uint32_t directCmd = makeVifCmd(0x50u, 0u, 2u);
    std::memcpy(rdram + kSrc, &directCmd, sizeof(directCmd));
    writeImagePacket(rdram, kSrc + 4u, 1u, 0x70u);
    rig.w32(kVif1Ch + 0x10u, kSrc);
    rig.w32(kVif1Ch + 0x20u, kQwc);
    rig.w32(kVif1Ch + 0x00u, 0x101u);
    REQUIRE(rig.sink.transfers.size() == 1u);
    CHECK(rig.sink.transfers[0].path == dq8::gs::GifPath::path2);
    CHECK(std::memcmp(rig.sink.transfers[0].bytes.data(), rdram + kSrc + 4u, 32u) == 0);
}

TEST_CASE("mmio devices: a GIF packet split across two DIRECT commands reaches the GS whole",
          "[mmio][vif][gif]") {
    // Upstream's "VIF1 DIRECT image tag can continue with raw image qwords" and "VIF1 DIRECT finds an image
    // continuation after packed setup" fed the image data after the DIRECT without a VIFcode, which the VIF
    // would decode as VIFcodes. The hardware-shaped version: the rest of the packet comes in a second DIRECT.
    EeRig rig;
    std::vector<uint8_t> packet;
    put32(packet, makeVifCmd(0x50u, 0u, 3u)); // PACKED tag + A+D + IMAGE tag
    put64(packet, makeGifTag(1u, GIF_FMT_PACKED, 1u, false));
    put64(packet, 0x0Eull);
    put64(packet, 0x8000008000ull);
    put64(packet, 0x3Bull); // TEXA
    put64(packet, makeGifTag(1u, GIF_FMT_IMAGE, 0u, true));
    put64(packet, 0ull);
    rig.vif1().push(packet);
    CHECK(rig.sink.transfers.empty()); // the IMAGE data has not arrived
    std::vector<uint8_t> rest;
    put32(rest, makeVifCmd(0x50u, 0u, 1u));
    for (uint32_t i = 0; i < 16u; ++i) {
        rest.push_back(static_cast<uint8_t>(0xC0u + i));
    }
    rig.vif1().push(rest);
    REQUIRE(rig.sink.transfers.size() == 1u);
    const auto& p = rig.sink.transfers[0].bytes;
    REQUIRE(p.size() == 64u);
    CHECK(p[48] == 0xC0u);
    CHECK(p[63] == 0xCFu);
}

// Upstream cases with no counterpart here, and why:
//   "translateAddress handles kseg and uncached aliases": no translateAddress; the aliases are checked
//     through loads and stores above and in test_guest_memory.cpp.
//   "scratchpad alias accesses the same bytes as base": 0xF0000000 is kseg3, TLB-mapped and unmapped for
//     the CPU in this runtime (M2 address routing); as a DMA address it is the scratchpad (checked above).
//   "native GIF packed chain matches generic packed primitive packet": upstream's GS fast path.
//   "VIF1 packet builders keep chain qwc live before terminate", "sceDmaReset re-enables DMAC DMAE",
//     "sceDmaSend preserves guest-configured VIF1 TTE": upstream's by-name library fakes; those libraries
//     are recompiled game code here (Docs/milestones/M3-boots.md, "Not reused").
//   "unaligned accesses throw": guest memory accepts any alignment; address errors are the recompiler's.
