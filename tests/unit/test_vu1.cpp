// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors: ps2xTest/src/ps2_vu1_tests.cpp.
// The cases and their expected values are upstream's; the harness is Catch2 over our VuInterpreter, with a
// capturing IVuHost where upstream used GS and PS2Memory, and the EE device rig where a case went through
// the VIF. Checks keep upstream's messages as comments.
#include "runtime/vu/vu_host.h"
#include "runtime/vu/vu_interpreter.h"
#include "support/ee_rig.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

using namespace dq8::rt;

namespace {

constexpr uint32_t PS2_VU1_CODE_SIZE = 0x4000u;
constexpr uint32_t PS2_VU1_DATA_SIZE = 0x4000u;
constexpr uint8_t GIF_FMT_IMAGE = 2u;
constexpr uint32_t kVuUpperNop = 0x000002FFu;

// Records XGKICK packets and reserved-instruction reports instead of a GS and PS2Memory.
struct CaptureHost final : IVuHost {
    void xgkick(std::span<const uint8_t> packet) override {
        packets.emplace_back(packet.begin(), packet.end());
    }
    uint64_t code_generation() const override { return generation; }
    void reserved_instruction(bool /*upper*/, uint32_t /*pc*/, uint32_t /*instruction*/) override {
        ++reserved;
    }

    std::vector<std::vector<uint8_t>> packets;
    uint64_t generation = 1;
    int reserved = 0;
};

struct Vu1Fixture {
    std::vector<uint8_t> code_mem = std::vector<uint8_t>(PS2_VU1_CODE_SIZE, 0u);
    std::vector<uint8_t> data_mem = std::vector<uint8_t>(PS2_VU1_DATA_SIZE, 0u);
    uint8_t* code = code_mem.data();
    uint8_t* data = data_mem.data();
    CaptureHost host;
};

uint32_t makeVifCmd(uint8_t opcode, uint8_t num, uint16_t imm) {
    return (static_cast<uint32_t>(opcode) << 24) | (static_cast<uint32_t>(num) << 16) |
           static_cast<uint32_t>(imm);
}

uint64_t makeGifTag(uint16_t nloop, uint8_t flg, uint8_t nreg, bool eop = true) {
    uint64_t tag = static_cast<uint64_t>(nloop & 0x7FFFu);
    if (eop)
        tag |= (1ull << 15);
    tag |= (static_cast<uint64_t>(flg & 0x3u) << 58);
    tag |= (static_cast<uint64_t>(nreg & 0xFu) << 60);
    return tag;
}

uint32_t makeVuLowerSpecial(uint8_t specialOp, uint8_t is, uint8_t it = 0u, uint8_t id = 0u,
                            uint8_t dest = 0u) {
    return (0x40u << 25) | (static_cast<uint32_t>(dest & 0xFu) << 21) |
           (static_cast<uint32_t>(it & 0x1Fu) << 16) | (static_cast<uint32_t>(is & 0x1Fu) << 11) |
           (static_cast<uint32_t>(id & 0x1Fu) << 6) | (static_cast<uint32_t>(specialOp & 0x7Cu) << 4) |
           static_cast<uint32_t>(specialOp & 0x3u) | 0x3Cu;
}

uint32_t makeVuLowerDirect(uint8_t funct, uint8_t is, uint8_t it = 0u, uint8_t id = 0u, uint8_t dest = 0u) {
    return (0x40u << 25) | (static_cast<uint32_t>(dest & 0xFu) << 21) |
           (static_cast<uint32_t>(it & 0x1Fu) << 16) | (static_cast<uint32_t>(is & 0x1Fu) << 11) |
           (static_cast<uint32_t>(id & 0x1Fu) << 6) | static_cast<uint32_t>(funct & 0x3Fu);
}

uint32_t makeVuUpper(uint8_t op, uint8_t dest, uint8_t ft, uint8_t fs, uint8_t fd) {
    return (static_cast<uint32_t>(dest & 0xFu) << 21) | (static_cast<uint32_t>(ft & 0x1Fu) << 16) |
           (static_cast<uint32_t>(fs & 0x1Fu) << 11) | (static_cast<uint32_t>(fd & 0x1Fu) << 6) |
           static_cast<uint32_t>(op & 0x3Fu);
}

uint32_t makeVuUpperSpecial(uint8_t specialOp, uint8_t dest, uint8_t ft, uint8_t fs) {
    return (static_cast<uint32_t>(dest & 0xFu) << 21) | (static_cast<uint32_t>(ft & 0x1Fu) << 16) |
           (static_cast<uint32_t>(fs & 0x1Fu) << 11) | (static_cast<uint32_t>(specialOp & 0x7Cu) << 4) |
           static_cast<uint32_t>(specialOp & 0x3u) | 0x3Cu;
}

uint32_t makeVuFlagImmediate(uint8_t opcode, uint8_t targetVi, uint16_t immediate) {
    return (static_cast<uint32_t>(opcode & 0x7Fu) << 25) |
           (static_cast<uint32_t>((immediate >> 11) & 0x1u) << 21) |
           (static_cast<uint32_t>(targetVi & 0xFu) << 16) | static_cast<uint32_t>(immediate & 0x7FFu);
}

uint32_t makeVuFlagRegister(uint8_t opcode, uint8_t targetVi, uint8_t sourceVi) {
    return (static_cast<uint32_t>(opcode & 0x7Fu) << 25) | (static_cast<uint32_t>(targetVi & 0xFu) << 16) |
           (static_cast<uint32_t>(sourceVi & 0xFu) << 11);
}

uint32_t makeVuLq(uint8_t dest, uint8_t targetVf, uint8_t baseVi, int16_t imm) {
    return (static_cast<uint32_t>(dest & 0xFu) << 21) | (static_cast<uint32_t>(targetVf & 0x1Fu) << 16) |
           (static_cast<uint32_t>(baseVi & 0xFu) << 11) | (static_cast<uint32_t>(imm) & 0x7FFu);
}

uint32_t makeVuSq(uint8_t dest, uint8_t sourceVf, uint8_t baseVi, int16_t imm) {
    return (0x01u << 25) | (static_cast<uint32_t>(dest & 0xFu) << 21) |
           (static_cast<uint32_t>(baseVi & 0xFu) << 16) | (static_cast<uint32_t>(sourceVf & 0x1Fu) << 11) |
           (static_cast<uint32_t>(imm) & 0x7FFu);
}

uint32_t makeVuIaddiu(uint8_t it, uint8_t is, int16_t imm) {
    return (0x08u << 25) | (static_cast<uint32_t>(it & 0xFu) << 16) |
           (static_cast<uint32_t>(is & 0xFu) << 11) | (static_cast<uint32_t>(imm) & 0x7FFu);
}

uint32_t makeVuBranch(int16_t imm) {
    return (0x20u << 25) | (static_cast<uint32_t>(imm) & 0x7FFu);
}

uint32_t makeVuJr(uint8_t is) {
    return (0x24u << 25) | (static_cast<uint32_t>(is & 0xFu) << 11);
}

uint32_t makeVuIbne(uint8_t is, uint8_t it, int16_t imm) {
    return (0x29u << 25) | (static_cast<uint32_t>(it & 0xFu) << 16) |
           (static_cast<uint32_t>(is & 0xFu) << 11) | (static_cast<uint32_t>(imm) & 0x7FFu);
}

uint32_t makeVuIlw(uint8_t dest, uint8_t targetVi, uint8_t baseVi, int16_t imm) {
    return (0x04u << 25) | (static_cast<uint32_t>(dest & 0xFu) << 21) |
           (static_cast<uint32_t>(targetVi & 0xFu) << 16) | (static_cast<uint32_t>(baseVi & 0xFu) << 11) |
           (static_cast<uint32_t>(imm) & 0x7FFu);
}

uint32_t makeVuDiv(uint8_t fs, uint8_t ft, uint8_t fsf, uint8_t ftf) {
    return makeVuLowerSpecial(0x38u, fs, ft, 0u, static_cast<uint8_t>(((ftf & 0x3u) << 2) | (fsf & 0x3u)));
}

uint32_t makeVuSqrt(uint8_t ft, uint8_t ftf) {
    return makeVuLowerSpecial(0x39u, 0u, ft, 0u, static_cast<uint8_t>((ftf & 0x3u) << 2));
}

void writeVuInstructionPair(uint8_t* code, uint32_t pc, uint32_t lower, uint32_t upper) {
    std::memcpy(code + pc, &lower, sizeof(lower));
    std::memcpy(code + pc + sizeof(lower), &upper, sizeof(upper));
}

uint64_t packVuInstructionPair(uint32_t lower, uint32_t upper) {
    return static_cast<uint64_t>(lower) | (static_cast<uint64_t>(upper) << 32);
}

void writeVuQword(uint8_t* data, uint32_t qwordIndex, const float values[4]) {
    std::memcpy(data + qwordIndex * 16u, values, sizeof(float) * 4u);
}

void readVuQword(const uint8_t* data, uint32_t qwordIndex, float values[4]) {
    std::memcpy(values, data + qwordIndex * 16u, sizeof(float) * 4u);
}

// Upstream wrote through PS2Memory, which bumps the VU1 code generation; the fixture host does the same.
void writeTrackedVuInstructionPair(Vu1Fixture& fx, uint32_t pc, uint32_t lower, uint32_t upper) {
    writeVuInstructionPair(fx.code, pc, lower, upper);
    ++fx.host.generation;
}

void appendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    const uint8_t* src = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), src, src + sizeof(value));
}

// MPG of one instruction pair through VIF1, as upstream's uploadVu1Mpg did through processVIF1Data.
void uploadVu1Mpg(dq8::test::EeRig& rig, uint16_t instructionAddress, uint32_t lower, uint32_t upper) {
    std::vector<uint8_t> packet;
    appendU32(packet, makeVifCmd(0x4Au, 1u, instructionAddress));
    appendU32(packet, lower);
    appendU32(packet, upper);
    rig.vif1().push(packet);
}

} // namespace

TEST_CASE("vu1: upper ADD applies the destination mask", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, 0u, makeVuUpper(0x28u, 0xAu, 2u, 1u, 3u)); // ADD.xz vf3, vf1, vf2

    VuInterpreter vu1;
    vu1.state().vf[1][0] = 1.0f;
    vu1.state().vf[1][1] = 2.0f;
    vu1.state().vf[1][2] = 3.0f;
    vu1.state().vf[1][3] = 4.0f;
    vu1.state().vf[2][0] = 10.0f;
    vu1.state().vf[2][1] = 20.0f;
    vu1.state().vf[2][2] = 30.0f;
    vu1.state().vf[2][3] = 40.0f;
    vu1.state().vf[3][0] = -1.0f;
    vu1.state().vf[3][1] = -2.0f;
    vu1.state().vf[3][2] = -3.0f;
    vu1.state().vf[3][3] = -4.0f;

    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 1u);

    CHECK((vu1.state().vf[3][0]) ==
          (-1.0f)); // FMAC destination must remain hidden before its four-cycle writeback
    vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 3u);
    CHECK((vu1.state().vf[3][0]) == (11.0f)); // ADD.x should write x
    CHECK((vu1.state().vf[3][1]) == (-2.0f)); // ADD.xz should preserve y
    CHECK((vu1.state().vf[3][2]) == (33.0f)); // ADD.xz should write z
    CHECK((vu1.state().vf[3][3]) == (-4.0f)); // ADD.xz should preserve w
}

TEST_CASE("vu1: LOI commits the lower immediate after the upper instruction", "[vu1]") {
    Vu1Fixture fx;

    const float newI = 7.0f;
    uint32_t lowerImmediate = 0u;
    std::memcpy(&lowerImmediate, &newI, sizeof(newI));
    const uint32_t upperAddiWithIBit =
        makeVuUpper(0x22u, 0xFu, 0u, 1u, 2u) | 0x80000000u; // ADDi.xyzw vf2, vf1
    writeVuInstructionPair(fx.code, 0u, lowerImmediate, upperAddiWithIBit);

    VuInterpreter vu1;
    vu1.state().i = 2.0f;
    vu1.state().vf[1][0] = 1.0f;
    vu1.state().vf[1][1] = 2.0f;
    vu1.state().vf[1][2] = 3.0f;
    vu1.state().vf[1][3] = 4.0f;

    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 1u);

    CHECK((vu1.state().vf[2][0]) == (0.0f)); // ADDi result should remain in the FMAC pipeline
    CHECK((vu1.state().i) == (7.0f));        // LOI should become visible after the upper from the same pair
    vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 3u);
    CHECK((vu1.state().vf[2][0]) == (3.0f)); // ADDi should use old I for x
    CHECK((vu1.state().vf[2][1]) == (4.0f)); // ADDi should use old I for y
    CHECK((vu1.state().vf[2][2]) == (5.0f)); // ADDi should use old I for z
    CHECK((vu1.state().vf[2][3]) == (6.0f)); // ADDi should use old I for w
    CHECK((vu1.state().i) == (7.0f));        // LOI should commit lower immediate into I after upper execution
}

TEST_CASE("vu1: ITOF converts the raw signed integer bits without float normalization", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, 0u, makeVuUpperSpecial(0x10u, 0xFu, 2u, 1u));

    VuInterpreter vu1;
    const int32_t raw[4] = {1, -16, 4096, -32768};
    std::memcpy(vu1.state().vf[1], raw, sizeof(raw));
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 4u);

    CHECK((vu1.state().vf[2][0]) ==
          (1.0f)); // ITOF0 must not flush an integer bit pattern that resembles a denormal
    CHECK((vu1.state().vf[2][1]) == (-16.0f));    // ITOF0 must preserve negative integer bit patterns
    CHECK((vu1.state().vf[2][2]) == (4096.0f));   // ITOF0 should convert positive fixed-point source bits
    CHECK((vu1.state().vf[2][3]) == (-32768.0f)); // ITOF0 should convert negative fixed-point source bits
}

TEST_CASE("vu1: MTIR decodes fsf as a component selector", "[vu1]") {
    Vu1Fixture fx;

    for (uint32_t component = 0; component < 4u; ++component) {
        writeVuInstructionPair(fx.code, component * 8u,
                               makeVuLowerSpecial(0x3Cu, 1u, static_cast<uint8_t>(component + 2u), 0u,
                                                  static_cast<uint8_t>(component)),
                               kVuUpperNop);
    }

    VuInterpreter vu1;
    const uint32_t raw[4] = {0x00001111u, 0x00002222u, 0x00003333u, 0x00004444u};
    std::memcpy(vu1.state().vf[1], raw, sizeof(raw));
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 4u);

    CHECK((vu1.state().vi[2]) == (0x1111)); // MTIR fsf=x should read VF.x
    CHECK((vu1.state().vi[3]) == (0x2222)); // MTIR fsf=y should read VF.y
    CHECK((vu1.state().vi[4]) == (0x3333)); // MTIR fsf=z should read VF.z
    CHECK((vu1.state().vi[5]) == (0x4444)); // MTIR fsf=w should read VF.w
}

TEST_CASE("vu1: LQ and SQ use VI qword addressing and destination masks", "[vu1]") {
    Vu1Fixture fx;

    const float sourceQw[4] = {10.0f, 20.0f, 30.0f, 40.0f};
    const float destQw[4] = {-1.0f, -2.0f, -3.0f, -4.0f};
    writeVuQword(fx.data, 3u, sourceQw);
    writeVuQword(fx.data, 5u, destQw);
    writeVuInstructionPair(fx.code, 0u, makeVuLq(0x5u, 4u, 1u, 1), kVuUpperNop); // LQ.yw vf4, 1(vi1)
    writeVuInstructionPair(fx.code, 8u, makeVuSq(0xAu, 4u, 2u, 1), kVuUpperNop); // SQ.xz vf4, 1(vi2)

    VuInterpreter vu1;
    vu1.state().vi[1] = 2;
    vu1.state().vi[2] = 4;
    vu1.state().vf[4][0] = 100.0f;
    vu1.state().vf[4][1] = 200.0f;
    vu1.state().vf[4][2] = 300.0f;
    vu1.state().vf[4][3] = 400.0f;

    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 3u);

    CHECK((vu1.state().vf[4][1]) == (200.0f)); // LQ result should remain hidden before its fourth cycle
    vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 1u);
    CHECK((vu1.state().vf[4][0]) == (100.0f)); // LQ.yw should preserve x
    CHECK((vu1.state().vf[4][1]) == (20.0f));  // LQ.yw should load y
    CHECK((vu1.state().vf[4][2]) == (300.0f)); // LQ.yw should preserve z
    CHECK((vu1.state().vf[4][3]) == (40.0f));  // LQ.yw should load w

    float stored[4] = {};
    readVuQword(fx.data, 5u, stored);
    CHECK((stored[0]) == (100.0f)); // SQ.xz should store x
    CHECK((stored[1]) == (-2.0f));  // SQ.xz should preserve y
    CHECK((stored[2]) == (300.0f)); // SQ.xz should store z
    CHECK((stored[3]) == (-4.0f));  // SQ.xz should preserve w
    CHECK((vu1.state().cycles) ==
          (static_cast<uint64_t>(4u))); // disjoint LQ/SQ lanes should issue without delaying LQ writeback
}

TEST_CASE("vu1: integer lower ops keep VI0 hardwired to zero", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, makeVuIaddiu(2u, 1u, 5), kVuUpperNop); // IADDIU vi2, vi1, 5
    writeVuInstructionPair(fx.code, 8u, makeVuIaddiu(0u, 2u, 7), kVuUpperNop); // IADDIU vi0, vi2, 7
    writeVuInstructionPair(fx.code, 16u, makeVuLowerDirect(0x30u, 2u, 1u, 3u),
                           kVuUpperNop); // IADD vi3, vi2, vi1

    VuInterpreter vu1;
    vu1.state().vi[0] = 99;
    vu1.state().vi[1] = 10;

    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 3u);

    CHECK((vu1.state().vi[2]) == (15)); // IADDIU should add signed immediate to VI source
    CHECK((vu1.state().vi[3]) == (25)); // IADD should add VI source registers
    CHECK((vu1.state().vi[0]) == (0));  // VI0 should remain hardwired to zero
}

TEST_CASE("vu1: XTOP and XITOP expose VIF TOP values to VI registers", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, makeVuLowerSpecial(0x68u, 0u, 2u), kVuUpperNop); // XTOP vi2
    writeVuInstructionPair(fx.code, 8u, makeVuLowerSpecial(0x69u, 0u, 3u), kVuUpperNop); // XITOP vi3

    VuInterpreter vu1;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0x123u, 0x2ABu, 2u);

    CHECK((vu1.state().vi[2]) == (0x123)); // XTOP should move TOP into the target VI register
    CHECK((vu1.state().vi[3]) == (0x2AB)); // XITOP should move ITOP into the target VI register
}

TEST_CASE("vu1: lower branch commits after one delay-slot instruction", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, makeVuBranch(2), kVuUpperNop);           // target pc = 24
    writeVuInstructionPair(fx.code, 8u, makeVuIaddiu(1u, 0u, 1), kVuUpperNop);   // delay slot
    writeVuInstructionPair(fx.code, 16u, makeVuIaddiu(2u, 0u, 99), kVuUpperNop); // skipped
    writeVuInstructionPair(fx.code, 24u, makeVuIaddiu(3u, 0u, 7), kVuUpperNop);  // branch target

    VuInterpreter vu1;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 3u);

    CHECK((vu1.state().vi[1]) == (1)); // branch delay slot should execute
    CHECK((vu1.state().vi[2]) == (0)); // instruction between delay slot and target should be skipped
    CHECK((vu1.state().vi[3]) == (7)); // branch target should execute after the delay slot
}

TEST_CASE("vu1: lower side sees old VF value when upper writes the same register", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, makeVuSq(0xFu, 1u, 1u, 0), // SQ.xyzw vf1, 0(vi1)
                           makeVuUpper(0x28u, 0xFu, 3u, 2u, 1u));  // ADD.xyzw vf1, vf2, vf3

    VuInterpreter vu1;
    vu1.state().vi[1] = 6;
    vu1.state().vf[1][0] = 1.0f;
    vu1.state().vf[1][1] = 2.0f;
    vu1.state().vf[1][2] = 3.0f;
    vu1.state().vf[1][3] = 4.0f;
    vu1.state().vf[2][0] = 10.0f;
    vu1.state().vf[2][1] = 20.0f;
    vu1.state().vf[2][2] = 30.0f;
    vu1.state().vf[2][3] = 40.0f;
    vu1.state().vf[3][0] = 100.0f;
    vu1.state().vf[3][1] = 200.0f;
    vu1.state().vf[3][2] = 300.0f;
    vu1.state().vf[3][3] = 400.0f;

    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 2u);

    float stored[4] = {};
    readVuQword(fx.data, 6u, stored);
    CHECK((stored[0]) == (1.0f)); // SQ should observe old VF value for x
    CHECK((stored[1]) == (2.0f)); // SQ should observe old VF value for y
    CHECK((stored[2]) == (3.0f)); // SQ should observe old VF value for z
    CHECK((stored[3]) == (4.0f)); // SQ should observe old VF value for w
    vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 2u);
    CHECK((vu1.state().vf[1][0]) == (110.0f)); // upper ADD should write x after lower read
    CHECK((vu1.state().vf[1][1]) == (220.0f)); // upper ADD should write y after lower read
    CHECK((vu1.state().vf[1][2]) == (330.0f)); // upper ADD should write z after lower read
    CHECK((vu1.state().vf[1][3]) == (440.0f)); // upper ADD should write w after lower read
}

TEST_CASE("vu1: upper suppresses only the colliding lower VF write", "[vu1]") {
    Vu1Fixture fx;

    const float loaded[4] = {90.0f, 91.0f, 92.0f, 93.0f};
    writeVuQword(fx.data, 5u, loaded);
    writeVuInstructionPair(fx.code, 0u, makeVuLowerSpecial(0x34u, 1u, 1u, 0u, 0xFu),
                           makeVuUpper(0x28u, 0x8u, 3u, 2u, 1u));

    VuInterpreter vu1;
    vu1.state().vi[1] = 5;
    vu1.state().vf[1][0] = 1.0f;
    vu1.state().vf[1][1] = 2.0f;
    vu1.state().vf[1][2] = 3.0f;
    vu1.state().vf[1][3] = 4.0f;
    vu1.state().vf[2][0] = 10.0f;
    vu1.state().vf[3][0] = 100.0f;

    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 1u);

    CHECK((vu1.state().vf[1][0]) ==
          (1.0f));                     // upper write should remain pending until the FMAC writeback cycle
    CHECK((vu1.state().vi[1]) == (6)); // LQI post-increment should commit after one cycle
    vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 3u);
    CHECK((vu1.state().vf[1][0]) == (110.0f)); // upper x should be written
    CHECK((vu1.state().vf[1][1]) == (2.0f));   // discarded lower must not leak y into the upper result
    CHECK((vu1.state().vf[1][2]) == (3.0f));   // discarded lower must not leak z into the upper result
    CHECK((vu1.state().vf[1][3]) == (4.0f));   // discarded lower must not leak w into the upper result
    CHECK((vu1.state().vi[1]) ==
          (6)); // LQI post-increment must survive suppression of its colliding VF write
}

TEST_CASE("vu1: upper and lower both read the pre-pair VF state", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, makeVuLowerSpecial(0x30u, 1u, 2u, 0u, 0x8u),
                           makeVuUpper(0x28u, 0x8u, 3u, 2u, 1u));

    VuInterpreter vu1;
    vu1.state().vf[1][0] = 10.0f;
    vu1.state().vf[2][0] = 20.0f;
    vu1.state().vf[3][0] = 1.0f;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 1u);

    vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 3u);
    CHECK((vu1.state().vf[1][0]) == (21.0f)); // upper must read vf2 before lower MOVE overwrites it
    CHECK((vu1.state().vf[2][0]) == (10.0f)); // lower must read vf1 before upper ADD overwrites it
}

TEST_CASE("vu1: FMAC dependency stalls only the lanes that are read", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, 0u, makeVuUpper(0x28u, 0x8u, 2u, 1u, 3u));
    writeVuInstructionPair(fx.code, 8u, 0u, makeVuUpper(0x28u, 0x8u, 2u, 3u, 4u));

    VuInterpreter vu1;
    vu1.state().vf[1][0] = 1.0f;
    vu1.state().vf[2][0] = 2.0f;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 5u);

    CHECK((vu1.state().vf[4][0]) ==
          (0.0f)); // the dependent result should remain pending until its own writeback
    vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 3u);
    CHECK((vu1.state().vf[4][0]) == (5.0f)); // dependent ADD should consume the completed x lane
    CHECK((vu1.state().cycles) ==
          (static_cast<uint64_t>(
              8u))); // dependency stall and the dependent FMAC writeback must both consume cycles
}

TEST_CASE("vu1: ACC forwarding feeds the next upper instruction without a dependency stall", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, 0u, makeVuUpperSpecial(0x28u, 0x8u, 2u, 1u)); // ADDA.x acc, vf1, vf2
    writeVuInstructionPair(fx.code, 8u, 0u, makeVuUpper(0x29u, 0x8u, 4u, 3u, 5u));    // MADD.x vf5, vf3, vf4

    VuInterpreter vu1;
    vu1.state().vf[1][0] = 1.0f;
    vu1.state().vf[2][0] = 10.0f;
    vu1.state().vf[3][0] = 2.0f;
    vu1.state().vf[4][0] = 3.0f;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 5u);

    CHECK((vu1.state().acc[0]) == (11.0f));   // ADDA should forward ACC to the following upper instruction
    CHECK((vu1.state().vf[5][0]) == (17.0f)); // MADD should consume the forwarded ACC value without stalling
    CHECK((vu1.state().cycles) ==
          (static_cast<uint64_t>(5u))); // ACC forwarding must not introduce a four-cycle dependency stall
}

TEST_CASE("vu1: ILW result becomes visible after four cycles before IALU consumes it", "[vu1]") {
    Vu1Fixture fx;

    const uint32_t source[4] = {0x1234u, 0u, 0u, 0u};
    std::memcpy(fx.data + 2u * 16u, source, sizeof(source));
    writeVuInstructionPair(fx.code, 0u, makeVuIlw(0x8u, 2u, 1u, 0), kVuUpperNop);
    writeVuInstructionPair(fx.code, 8u, makeVuIaddiu(3u, 2u, 1), kVuUpperNop);

    VuInterpreter vu1;
    vu1.state().vi[1] = 2;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 5u);

    CHECK((vu1.state().vi[2]) == (0x1234)); // ILW should commit the selected word after four cycles
    CHECK((vu1.state().vi[3]) == (0x1235)); // IADDIU should wait for and consume the ILW result
    CHECK((vu1.state().cycles) ==
          (static_cast<uint64_t>(5u))); // ILW-to-IALU dependency should account for all stalled cycles
}

TEST_CASE("vu1: DIV and SQRT update the Q register from selected vector components", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, makeVuDiv(1u, 2u, 1u, 2u), kVuUpperNop);      // Q = vf1.y / vf2.z
    writeVuInstructionPair(fx.code, 8u, makeVuLowerSpecial(0x3Bu, 0u), kVuUpperNop);  // WAITQ
    writeVuInstructionPair(fx.code, 16u, makeVuSqrt(3u, 3u), kVuUpperNop);            // Q = sqrt(abs(vf3.w))
    writeVuInstructionPair(fx.code, 24u, makeVuLowerSpecial(0x3Bu, 0u), kVuUpperNop); // WAITQ

    VuInterpreter vu1;
    vu1.state().vf[1][1] = 18.0f;
    vu1.state().vf[2][2] = 3.0f;
    vu1.state().vf[3][3] = 25.0f;

    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 8u);
    CHECK((vu1.state().q) == (6.0f)); // WAITQ should expose DIV after its seven-cycle latency
    CHECK((vu1.state().cycles) ==
          (static_cast<uint64_t>(8u))); // maxCycles should count FDIV stalls as elapsed VU cycles

    vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 8u);
    CHECK((vu1.state().q) == (5.0f)); // WAITQ should expose SQRT after its seven-cycle latency
}

TEST_CASE("vu1: FDIV resource serializes back-to-back scalar operations", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, makeVuDiv(1u, 2u, 0u, 0u),
                           kVuUpperNop); // Q = vf1.x / vf2.x
    writeVuInstructionPair(fx.code, 8u, makeVuSqrt(3u, 0u),
                           kVuUpperNop); // Must wait for the shared FDIV unit.
    writeVuInstructionPair(fx.code, 16u, makeVuLowerSpecial(0x3Bu, 0u),
                           kVuUpperNop); // WAITQ

    VuInterpreter vu1;
    vu1.state().vf[1][0] = 18.0f;
    vu1.state().vf[2][0] = 3.0f;
    vu1.state().vf[3][0] = 25.0f;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 15u);

    CHECK((vu1.state().q) == (5.0f)); // the second FDIV operation should commit the final Q value
    CHECK((vu1.state().cycles) ==
          (static_cast<uint64_t>(15u))); // back-to-back FDIV operations should include the resource stall
}

TEST_CASE("vu1: FDIV commits current and sticky divide-invalid status", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, makeVuDiv(1u, 2u, 0u, 0u), kVuUpperNop);
    writeVuInstructionPair(fx.code, 8u, makeVuLowerSpecial(0x3Bu, 0u), kVuUpperNop);

    VuInterpreter vu1;
    vu1.state().vf[1][0] = 0.0f;
    vu1.state().vf[2][0] = 0.0f;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 8u);
    CHECK((vu1.state().status) == (0x410u)); // zero divided by zero should set current and sticky I

    vu1.reset();
    vu1.state().vf[1][0] = 1.0f;
    vu1.state().vf[2][0] = 0.0f;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 8u);
    CHECK((vu1.state().status) ==
          (0x820u)); // a nonzero numerator divided by zero should set current and sticky D
}

TEST_CASE("vu1: EFU WAITP and RNG execute with architectural latency and state", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, makeVuLowerSpecial(0x70u, 1u),
                           kVuUpperNop); // ESADD P, vf1
    writeVuInstructionPair(fx.code, 8u, makeVuLowerSpecial(0x7Bu, 0u),
                           kVuUpperNop); // WAITP
    writeVuInstructionPair(fx.code, 16u, makeVuLowerSpecial(0x42u, 2u, 0u, 0u, 0x8u),
                           kVuUpperNop); // RINIT R, vf2.x
    writeVuInstructionPair(fx.code, 24u, makeVuLowerSpecial(0x40u, 0u, 3u, 0u, 0x8u),
                           kVuUpperNop); // RNEXT.x vf3

    VuInterpreter vu1;
    vu1.state().vf[1][0] = 1.0f;
    vu1.state().vf[1][1] = 2.0f;
    vu1.state().vf[1][2] = 3.0f;
    vu1.state().vf[2][0] = 1.5f;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 14u);

    CHECK((vu1.state().p) == (14.0f));       // WAITP should expose ESADD after eleven cycles
    CHECK((vu1.state().vf[3][0]) == (0.0f)); // RNEXT vector result should respect FMAC writeback latency
    vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 3u);
    CHECK((vu1.state().vf[3][0] >= 1.0f && vu1.state().vf[3][0] < 2.0f &&
           vu1.state().vf[3][0] != 1.5f)); // RNEXT should advance the 23-bit R LFSR and write a 1.x value
}

TEST_CASE("vu1: EFU resource observes throughput separately from P visibility", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, makeVuLowerSpecial(0x70u, 1u),
                           kVuUpperNop); // ESADD: result at cycle 11, resource free at 10.
    writeVuInstructionPair(fx.code, 8u, makeVuLowerSpecial(0x72u, 1u),
                           kVuUpperNop); // ELENG: must issue at cycle 10.
    writeVuInstructionPair(fx.code, 16u, makeVuLowerSpecial(0x7Bu, 0u),
                           kVuUpperNop); // WAITP waits for ELENG at cycle 28.

    VuInterpreter vu1;
    vu1.state().vf[1][0] = 1.0f;
    vu1.state().vf[1][1] = 2.0f;
    vu1.state().vf[1][2] = 3.0f;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 29u);

    CHECK((vu1.state().p > 3.7f && vu1.state().p < 3.8f)); // WAITP should expose the second EFU result
    CHECK((vu1.state().cycles) ==
          (static_cast<uint64_t>(29u))); // EFU scheduling should use opcode throughput and result latency
}

TEST_CASE("vu1: all EFU opcodes produce P at their architectural latency", "[vu1]") {
    Vu1Fixture fx;

    struct EfuCase {
        uint8_t opcode;
        uint32_t latency;
    };
    constexpr EfuCase cases[] = {{0x70u, 11u}, {0x71u, 18u}, {0x72u, 18u}, {0x73u, 24u}, {0x74u, 54u},
                                 {0x75u, 54u}, {0x76u, 12u}, {0x77u, 18u}, {0x78u, 12u}, {0x79u, 29u},
                                 {0x7Au, 12u}, {0x7Cu, 54u}, {0x7Du, 44u}};

    for (const EfuCase& efu : cases) {
        std::memset(fx.code, 0, PS2_VU1_CODE_SIZE);
        writeVuInstructionPair(fx.code, 0u, makeVuLowerSpecial(efu.opcode, 1u, 0u, 0u, 0u), kVuUpperNop);
        writeVuInstructionPair(fx.code, 8u, makeVuLowerSpecial(0x7Bu, 0u), kVuUpperNop);

        VuInterpreter vu1;
        vu1.state().vf[1][0] = 0.25f;
        vu1.state().vf[1][1] = 0.5f;
        vu1.state().vf[1][2] = 0.75f;
        vu1.state().vf[1][3] = 1.0f;
        vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u,
                    efu.latency + 1u);

        CHECK((vu1.state().cycles) ==
              (static_cast<uint64_t>(efu.latency +
                                     1u)));  // WAITP should count every EFU stall as an elapsed VU cycle
        CHECK(std::isfinite(vu1.state().p)); // architected EFU opcode should commit a finite P result
    }
}

TEST_CASE("vu1: E and enabled D/T stop with their architectural delay rules", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, 0u, kVuUpperNop | 0x40000000u);
    writeVuInstructionPair(fx.code, 8u, makeVuIaddiu(1u, 0u, 7), kVuUpperNop);
    writeVuInstructionPair(fx.code, 16u, makeVuIaddiu(2u, 0u, 9), kVuUpperNop);

    VuInterpreter vu1;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 32u);
    CHECK((vu1.state().vi[1]) == (7)); // E should execute exactly one sequential delay slot
    CHECK((vu1.state().vi[2]) == (0)); // E should stop before the instruction after its delay slot

    vu1.reset();
    writeTrackedVuInstructionPair(fx, 0u, makeVuIaddiu(1u, 0u, 3), kVuUpperNop | 0x08000000u);
    writeTrackedVuInstructionPair(fx, 8u, makeVuIaddiu(2u, 0u, 5), kVuUpperNop);
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 2u);
    CHECK((vu1.state().vi[2]) == (5)); // T must be ignored while TE is disabled

    vu1.reset();
    vu1.state().tBitEnabled = true;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 32u);
    CHECK((vu1.state().vi[1]) == (3)); // T instruction itself should complete
    CHECK((vu1.state().vi[2]) == (0)); // enabled T should stop without an ordinary delay slot
    CHECK(vu1.state().stoppedByT);     // the stop reason should identify T

    vu1.reset();
    vu1.state().dBitEnabled = true;
    writeTrackedVuInstructionPair(fx, 0u, makeVuIaddiu(1u, 0u, 4), kVuUpperNop | 0x10000000u);
    writeTrackedVuInstructionPair(fx, 8u, makeVuIaddiu(2u, 0u, 6), kVuUpperNop);
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 32u);
    CHECK((vu1.state().vi[1]) == (4)); // D instruction itself should complete
    CHECK((vu1.state().vi[2]) == (0)); // enabled D should stop without an ordinary delay slot
    CHECK(vu1.state().stoppedByD);     // the stop reason should identify D

    vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 1u);
    CHECK((vu1.state().vi[2]) == (6)); // MSCNT-style resume should continue at the stopped TPC
    CHECK((!vu1.state().stoppedByD &&
           !vu1.state().stoppedByT)); // resuming should clear the previous D/T stop reason

    vu1.reset();
    vu1.state().tBitEnabled = true;
    writeTrackedVuInstructionPair(fx, 0u, makeVuBranch(1), kVuUpperNop | 0x08000000u);
    writeTrackedVuInstructionPair(fx, 8u, makeVuIaddiu(2u, 0u, 11), kVuUpperNop);
    writeTrackedVuInstructionPair(fx, 16u, makeVuIaddiu(3u, 0u, 13), kVuUpperNop);
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 32u);
    CHECK((vu1.state().vi[2]) == (11)); // T on a branch should still execute its branch delay slot
    CHECK((vu1.state().vi[3]) == (0));  // T on a branch should stop before executing the branch target
    CHECK((vu1.state().pc) == (16u));   // the stopped TPC should be the branch destination
}

TEST_CASE("vu1: conditional branch sees the previous VI value for one instruction", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, makeVuIaddiu(1u, 0u, 1), kVuUpperNop);
    writeVuInstructionPair(fx.code, 8u, makeVuIbne(1u, 0u, 2), kVuUpperNop);
    writeVuInstructionPair(fx.code, 16u, makeVuIaddiu(2u, 0u, 2), kVuUpperNop);
    writeVuInstructionPair(fx.code, 24u, makeVuIaddiu(3u, 0u, 3), kVuUpperNop);
    writeVuInstructionPair(fx.code, 32u, makeVuIaddiu(4u, 0u, 4), kVuUpperNop);

    VuInterpreter vu1;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 4u);

    CHECK((vu1.state().vi[2]) == (2)); // the instruction after a conditional branch remains its delay slot
    CHECK((vu1.state().vi[3]) == (3)); // an immediately following branch should see the pre-write VI value

    vu1.reset();
    writeTrackedVuInstructionPair(fx, 8u, 0u, kVuUpperNop);
    writeTrackedVuInstructionPair(fx, 16u, makeVuIbne(1u, 0u, 2), kVuUpperNop);
    writeTrackedVuInstructionPair(fx, 40u, makeVuIaddiu(5u, 0u, 5), kVuUpperNop);
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 5u);
    CHECK((vu1.state().vi[3]) == (3)); // taken branch should still execute its delay slot
    CHECK((vu1.state().vi[4]) ==
          (0)); // after one intervening instruction the branch should observe and branch on the new VI value
    CHECK((vu1.state().vi[5]) == (5)); // taken branch should arrive at its target after the delay slot

    vu1.reset();
    writeTrackedVuInstructionPair(fx, 0u, makeVuIaddiu(1u, 0u, 1), makeVuUpper(0x28u, 0x8u, 2u, 1u, 3u));
    writeTrackedVuInstructionPair(fx, 8u, makeVuIbne(1u, 0u, 2), makeVuUpper(0x28u, 0x8u, 0u, 3u, 4u));
    writeTrackedVuInstructionPair(fx, 16u, makeVuIaddiu(2u, 0u, 2), kVuUpperNop);
    writeTrackedVuInstructionPair(fx, 24u, makeVuIaddiu(3u, 0u, 3), kVuUpperNop);
    writeTrackedVuInstructionPair(fx, 32u, makeVuIaddiu(4u, 0u, 4), kVuUpperNop);
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 7u);
    CHECK((vu1.state().vi[2]) == (2)); // a stalled conditional branch should retain its delay slot
    CHECK((vu1.state().vi[3]) ==
          (3)); // the VI bypass must survive VF hazard stalls before the next pair issues
    CHECK((vu1.state().vi[4]) == (0)); // elapsed stall cycles must not expire the one-instruction VI bypass
}

TEST_CASE("vu1: flag checks are visible to an immediately following branch", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, makeVuFlagImmediate(0x16u, 1u, 0x001u), kVuUpperNop);
    writeVuInstructionPair(fx.code, 8u, makeVuIbne(1u, 0u, 2), kVuUpperNop);
    writeVuInstructionPair(fx.code, 16u, makeVuIaddiu(2u, 0u, 2), kVuUpperNop);
    writeVuInstructionPair(fx.code, 24u, makeVuIaddiu(3u, 0u, 3), kVuUpperNop);
    writeVuInstructionPair(fx.code, 32u, makeVuIaddiu(4u, 0u, 4), kVuUpperNop);

    VuInterpreter vu1;
    vu1.state().status = 0x001u;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 4u);

    CHECK((vu1.state().vi[1]) == (1)); // FSAND should publish its result after one cycle
    CHECK((vu1.state().vi[2]) == (2)); // the taken branch should still execute its delay slot
    CHECK((vu1.state().vi[3]) == (0)); // the immediately following branch must consume the flag-check result
    CHECK((vu1.state().vi[4]) == (4)); // the flag-driven branch should arrive at its target
}

TEST_CASE("vu1: JR shares the one-instruction VI branch visibility rule", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, makeVuIaddiu(1u, 0u, 2), kVuUpperNop);
    writeVuInstructionPair(fx.code, 8u, makeVuJr(1u), kVuUpperNop);
    writeVuInstructionPair(fx.code, 16u, makeVuIaddiu(2u, 0u, 2), kVuUpperNop);
    writeVuInstructionPair(fx.code, 24u, makeVuIaddiu(3u, 0u, 3), kVuUpperNop);
    writeVuInstructionPair(fx.code, 32u, makeVuIaddiu(4u, 0u, 4), kVuUpperNop);

    VuInterpreter vu1;
    vu1.state().vi[1] = 4;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 4u);

    CHECK((vu1.state().vi[1]) == (2)); // the pending IALU write should still commit normally
    CHECK((vu1.state().vi[2]) == (2)); // JR should execute exactly one delay-slot pair
    CHECK((vu1.state().vi[3]) == (0)); // JR should skip the sequential instruction after its delay slot
    CHECK((vu1.state().vi[4]) ==
          (4)); // JR immediately after a VI write should branch using the previous VI value
}

TEST_CASE("vu1: MPG upload invalidates cached VU1 decode before MSCAL", "[vu1]") {
    // MSCAL runs the program to its end here, so each upload carries an E bit on the upper instruction.
    dq8::test::EeRig rig;
    constexpr uint32_t ebit = 1u << 30;
    uploadVu1Mpg(rig, 0u, makeVuIaddiu(1u, 0u, 1), kVuUpperNop | ebit);
    const uint32_t firstMscal = makeVifCmd(0x14u, 0u, 0u);
    rig.vif1().push(std::span(reinterpret_cast<const uint8_t*>(&firstMscal), sizeof(firstMscal)));
    CHECK((rig.vu().interpreter(1).state().vi[1]) ==
          (1)); // first MSCAL should execute the first uploaded program

    uploadVu1Mpg(rig, 0u, makeVuIaddiu(1u, 0u, 2), kVuUpperNop | ebit);
    const uint32_t secondMscal = makeVifCmd(0x14u, 0u, 0u);
    rig.vif1().push(std::span(reinterpret_cast<const uint8_t*>(&secondMscal), sizeof(secondMscal)));
    CHECK((rig.vu().interpreter(1).state().vi[1]) ==
          (2)); // second MSCAL should see the MPG-updated instruction
}

TEST_CASE("vu1: direct VU1 code writes invalidate cached decode", "[vu1]") {
    // EE stores to VU1 micro memory (0x11008000) through the MMIO bus.
    dq8::test::EeRig rig;
    VuUnits& vu = rig.vu();
    VuInterpreter vu1;
    rig.w64(0x11008000u, packVuInstructionPair(makeVuIaddiu(1u, 0u, 1), kVuUpperNop));
    vu1.execute(vu.micro(1).data(), PS2_VU1_CODE_SIZE, vu.data(1).data(), PS2_VU1_DATA_SIZE, vu.host(1), 0u,
                0u, 0u, 1u);
    CHECK((vu1.state().vi[1]) == (1)); // first execution should use the original direct write

    rig.w64(0x11008000u, packVuInstructionPair(makeVuIaddiu(1u, 0u, 2), kVuUpperNop));
    vu1.execute(vu.micro(1).data(), PS2_VU1_CODE_SIZE, vu.data(1).data(), PS2_VU1_DATA_SIZE, vu.host(1), 0u,
                0u, 0u, 1u);
    CHECK((vu1.state().vi[1]) == (2)); // second execution should rebuild decode after the direct write
}

TEST_CASE("vu1: XGKICK sends a VU memory GIF packet through PATH1", "[vu1]") {
    Vu1Fixture fx;
    auto& captured = fx.host.packets;

    uint8_t* vuCode = fx.code;
    uint8_t* vuData = fx.data;
    std::memset(vuCode, 0, PS2_VU1_CODE_SIZE);
    std::memset(vuData, 0, PS2_VU1_DATA_SIZE);

    constexpr uint32_t kLastQw = (PS2_VU1_DATA_SIZE / 16u) - 1u;
    const uint32_t tagOffset = kLastQw * 16u;

    const uint64_t imageTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
    std::memcpy(vuData + tagOffset, &imageTag, sizeof(imageTag));

    for (uint32_t i = 0; i < 16u; ++i) {
        vuData[i] = static_cast<uint8_t>(0xC0u + i);
    }

    const uint32_t lower = makeVuLowerSpecial(0x6Cu, 1u);
    std::memcpy(vuCode + 0u, &lower, sizeof(lower));
    const uint32_t upper = 0u;
    std::memcpy(vuCode + 4u, &upper, sizeof(upper));

    VuInterpreter vu1;
    vu1.state().vi[1] = static_cast<int32_t>(kLastQw);
    vu1.execute(vuCode, PS2_VU1_CODE_SIZE, vuData, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 3u);

    CHECK((captured.size()) == (static_cast<size_t>(1u))); // XGKICK should emit one wrapped GIF packet
    if (!captured.empty()) {
        CHECK((captured[0].size()) ==
              (static_cast<size_t>(32u))); // wrapped packet should include tag plus one qword payload
        bool payloadOk = true;
        for (uint32_t i = 0; i < 16u; ++i) {
            if (captured[0].size() < 32u || captured[0][16u + i] != static_cast<uint8_t>(0xC0u + i)) {
                payloadOk = false;
                break;
            }
        }
        CHECK(payloadOk); // wrapped payload should be copied from start of VU1 memory
    }
}

TEST_CASE("vu1: XGKICK observes stores committed before a future PATH1 qword", "[vu1]") {
    Vu1Fixture fx;
    auto& captured = fx.host.packets;

    uint8_t* code = fx.code;
    uint8_t* data = fx.data;
    std::memset(code, 0, PS2_VU1_CODE_SIZE);
    std::memset(data, 0xFF, PS2_VU1_DATA_SIZE);

    const uint64_t imageTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
    std::memset(data, 0, 16u);
    std::memcpy(data, &imageTag, sizeof(imageTag));
    writeVuInstructionPair(code, 0u, makeVuLowerSpecial(0x6Cu, 1u), kVuUpperNop);
    writeVuInstructionPair(code, 8u, makeVuSq(0xFu, 4u, 2u, 0), kVuUpperNop);
    writeVuInstructionPair(code, 16u, 0u, kVuUpperNop);

    VuInterpreter vu1;
    vu1.state().vi[1] = 0;
    vu1.state().vi[2] = 1;
    const float replacement[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    std::memcpy(vu1.state().vf[4], replacement, sizeof(replacement));
    vu1.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 3u);

    CHECK((captured.size()) ==
          (static_cast<size_t>(1u))); // PATH1 should finish after consuming the updated payload
    if (!captured.empty()) {
        CHECK((captured[0].size() >= 32u &&
               std::memcmp(captured[0].data() + 16u, replacement,
                           sizeof(replacement)) == 0)); // XGKICK must read each qword in its transfer cycle
    }
}

TEST_CASE("vu1: a second XGKICK stalls until the active PATH1 transfer completes", "[vu1]") {
    Vu1Fixture fx;
    auto& captured = fx.host.packets;
    uint8_t* code = fx.code;
    uint8_t* data = fx.data;
    std::memset(code, 0, PS2_VU1_CODE_SIZE);
    std::memset(data, 0, PS2_VU1_DATA_SIZE);

    const uint64_t imageTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
    std::memcpy(data + 0u, &imageTag, sizeof(imageTag));
    std::memcpy(data + 32u, &imageTag, sizeof(imageTag));
    std::memset(data + 16u, 0x11, 16u);
    std::memset(data + 48u, 0x22, 16u);
    writeVuInstructionPair(code, 0u, makeVuLowerSpecial(0x6Cu, 1u), kVuUpperNop);
    writeVuInstructionPair(code, 8u, makeVuLowerSpecial(0x6Cu, 2u), kVuUpperNop);
    writeVuInstructionPair(code, 16u, 0u, kVuUpperNop);
    writeVuInstructionPair(code, 24u, 0u, kVuUpperNop);

    VuInterpreter vu1;
    vu1.state().vi[1] = 0;
    vu1.state().vi[2] = 2;
    vu1.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 6u);

    CHECK((captured.size()) ==
          (static_cast<size_t>(2u))); // both PATH1 transfers should complete in issue order
    if (captured.size() == 2u) {
        CHECK((captured[0].size() >= 32u &&
               captured[0][16u] == 0x11u)); // the first XGKICK payload should be delivered first
        CHECK((captured[1].size() >= 32u &&
               captured[1][16u] == 0x22u)); // the stalled XGKICK should retain its own source address
    }
    CHECK((vu1.state().cycles) ==
          (static_cast<uint64_t>(6u))); // XGKICK resource stalls should consume VU cycles
}

TEST_CASE("vu1: synthetic Code Veronica text packet preserves black-frame PATH1 data", "[vu1]") {
    Vu1Fixture fx;
    auto& captured = fx.host.packets;

    uint8_t* code = fx.code;
    uint8_t* data = fx.data;
    std::memset(code, 0, PS2_VU1_CODE_SIZE);
    std::memset(data, 0, PS2_VU1_DATA_SIZE);

    const uint64_t imageTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
    std::memcpy(data, &imageTag, sizeof(imageTag));
    writeVuInstructionPair(code, 0u, 0u, makeVuUpper(0x28u, 0xFu, 3u, 2u, 4u));
    writeVuInstructionPair(code, 8u, makeVuSq(0xFu, 4u, 2u, 0), kVuUpperNop);
    writeVuInstructionPair(code, 16u, makeVuLowerSpecial(0x6Cu, 1u), kVuUpperNop);

    VuInterpreter vu1;
    vu1.state().vi[1] = 0;
    vu1.state().vi[2] = 1;
    const float a[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float b[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    std::memcpy(vu1.state().vf[2], a, sizeof(a));
    std::memcpy(vu1.state().vf[3], b, sizeof(b));

    vu1.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 10u);

    CHECK((captured.size()) ==
          (static_cast<size_t>(1u))); // the synthetic scene should emit exactly one PATH1 packet
    if (!captured.empty()) {
        const float expected[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        CHECK((captured[0].size()) ==
              (static_cast<size_t>(32u))); // text regression packet should contain one image qword
        CHECK((captured[0].size() >= 32u &&
               std::memcmp(captured[0].data() + 16u, expected,
                           sizeof(expected)) ==
                   0)); // PATH1 must observe the post-FMAC store, not stale blue/magenta data
    }
}

TEST_CASE("vu1: MSCAL can start a VU1 XGKICK program and update GS VRAM", "[vu1]") {
    // There is no GS model here: the check is that the PATH1 packet MSCAL's program kicks reaches the GS sink
    // whole (upstream checked the uploaded pixels in its GS VRAM). The upper carries an E bit so the program
    // ends.
    dq8::test::EeRig rig;
    uint8_t* vuCode = rig.vu().micro(1).data();
    uint8_t* vuData = rig.vu().data(1).data();

    const uint32_t lower = makeVuLowerSpecial(0x6Cu, 0u);
    std::memcpy(vuCode + 0u, &lower, sizeof(lower));
    const uint32_t upper = 1u << 30;
    std::memcpy(vuCode + 4u, &upper, sizeof(upper));
    rig.vu().touch_micro(1);

    const uint64_t gifTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
    std::memcpy(vuData + 0u, &gifTag, sizeof(gifTag));
    const uint64_t tagHi = 0u;
    std::memcpy(vuData + 8u, &tagHi, sizeof(tagHi));
    for (uint32_t i = 0; i < 16u; ++i) {
        vuData[16u + i] = static_cast<uint8_t>(0x90u + i);
    }

    const uint32_t mscalCmd = makeVifCmd(0x14u, 0u, 0u);
    rig.vif1().push(std::span(reinterpret_cast<const uint8_t*>(&mscalCmd), sizeof(mscalCmd)));

    REQUIRE(rig.sink.transfers.size() ==
            1u); // MSCAL-triggered XGKICK should route one PATH1 packet to the GS
    const auto& packet = rig.sink.transfers[0];
    CHECK(packet.path == dq8::gs::GifPath::path1);
    REQUIRE(packet.bytes.size() == 32u);
    CHECK(std::memcmp(packet.bytes.data(), &gifTag, sizeof(gifTag)) == 0);
    bool imageOk = true;
    for (uint32_t i = 0; i < 16u; ++i) {
        imageOk = imageOk && packet.bytes[16u + i] == static_cast<uint8_t>(0x90u + i);
    }
    CHECK(imageOk); // the image qword arrives unchanged
}

TEST_CASE("vu1: standalone VU1 code honors the nullable PS2Memory API", "[vu1]") {
    // Upstream checked that a null PS2Memory was tolerated; the equivalent here is code outside VU memory
    // driven through a plain host.
    std::vector<uint8_t> code(8u, 0u);
    std::vector<uint8_t> data(16u, 0u);
    CaptureHost host;

    VuInterpreter vu1;
    vu1.execute(code.data(), static_cast<uint32_t>(code.size()), data.data(),
                static_cast<uint32_t>(data.size()), host, 0u, 0u, 0u, 1u);

    CHECK((vu1.state().pc) == (0u)); // external code should execute and wrap
}

TEST_CASE("vu1: VU1 status-immediate ops decode IMM12 and their target VI", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, makeVuFlagImmediate(0x14u, 5u, 0x812u), kVuUpperNop);
    writeVuInstructionPair(fx.code, 8u, makeVuFlagImmediate(0x16u, 6u, 0x810u), kVuUpperNop);
    writeVuInstructionPair(fx.code, 16u, makeVuFlagImmediate(0x17u, 7u, 0x040u), kVuUpperNop);

    VuInterpreter vu1;
    vu1.state().status = 0x812u;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 3u);

    CHECK((vu1.state().vi[5]) == (1));     // FSEQ should compare all 12 immediate bits and write IT
    CHECK((vu1.state().vi[6]) == (0x810)); // FSAND should return the masked 12-bit status in IT
    CHECK((vu1.state().vi[7]) == (0x852)); // FSOR should return the 12-bit OR value rather than a boolean
    CHECK((vu1.state().vi[1]) == (0));     // status-immediate ops should not hardcode VI1
}

TEST_CASE("vu1: VU1 FSSET enters the four-cycle flag pipeline", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, makeVuFlagImmediate(0x15u, 0u, 0xA80u), kVuUpperNop);

    VuInterpreter vu1;
    vu1.state().status = 0x015u;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 3u);
    CHECK((vu1.state().status) == (0x015u)); // FSSET should not be visible before four cycles elapse

    vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 1u);
    CHECK((vu1.state().status) ==
          (0xA95u)); // FSSET should replace sticky bits while preserving current and D/I bits
}

TEST_CASE("vu1: VU1 has one flag pipeline and FSSET wins a same-pair conflict", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, makeVuFlagImmediate(0x15u, 0u, 0xA80u),
                           makeVuUpper(0x28u, 0xAu, 2u, 1u, 3u));
    for (uint32_t pc = 8u; pc <= 32u; pc += 8u)
        writeVuInstructionPair(fx.code, pc, 0u, kVuUpperNop);

    VuInterpreter vu1;
    vu1.state().vf[1][0] = 1.0f;
    vu1.state().vf[1][2] = -3.0f;
    vu1.state().vf[2][0] = -1.0f;
    vu1.state().vf[2][2] = 1.0f;

    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 5u);

    CHECK((vu1.state().mac) ==
          (0x28u)); // same-pair FSSET should suppress only STATUS, not the upper MAC result
    CHECK((vu1.state().status) == (0xA80u)); // the single runtime pipeline should commit FSSET sticky bits
}

TEST_CASE("vu1: VU1 FMAC flags respect destination lanes and become visible after four cycles", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, 0u, makeVuUpper(0x28u, 0xAu, 2u, 1u, 3u));
    writeVuInstructionPair(fx.code, 8u, makeVuFlagRegister(0x18u, 6u, 7u), kVuUpperNop);
    writeVuInstructionPair(fx.code, 16u, 0u, kVuUpperNop);
    writeVuInstructionPair(fx.code, 24u, 0u, kVuUpperNop);
    writeVuInstructionPair(fx.code, 32u, makeVuFlagRegister(0x1Au, 4u, 5u), kVuUpperNop);

    VuInterpreter vu1;
    vu1.state().vf[1][0] = 1.0f;
    vu1.state().vf[1][2] = -3.0f;
    vu1.state().vf[2][0] = -1.0f;
    vu1.state().vf[2][2] = 1.0f;
    vu1.state().vi[5] = 0xFFFF;
    vu1.state().vi[7] = 0;

    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 3u);

    CHECK((vu1.state().mac) == (0u));  // FMAC flags should remain hidden before four cycles elapse
    CHECK((vu1.state().vi[6]) == (1)); // FMEQ before the commit cycle should observe the old MAC flags

    vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 2u);

    CHECK((vu1.state().mac) == (0x28u)); // ADD.xz should report zero on x and sign on z only
    CHECK((vu1.state().status) ==
          (0xC3u)); // FMAC commit should update current Z/S and accumulate their sticky bits
    CHECK((vu1.state().vi[4]) == (0x28)); // FMAND on the commit cycle should observe the new MAC flags
}

TEST_CASE("vu1: VU1 CLIP and FCGET share the four-cycle flag timeline", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, 0u, makeVuUpperSpecial(0x1Fu, 0u, 2u, 1u));
    writeVuInstructionPair(fx.code, 8u, makeVuFlagRegister(0x1Cu, 3u, 0u), kVuUpperNop);
    writeVuInstructionPair(fx.code, 16u, 0u, kVuUpperNop);
    writeVuInstructionPair(fx.code, 24u, 0u, kVuUpperNop);
    writeVuInstructionPair(fx.code, 32u, makeVuFlagRegister(0x1Cu, 4u, 0u), kVuUpperNop);

    VuInterpreter vu1;
    vu1.state().vf[1][0] = 2.0f;
    vu1.state().vf[2][3] = 1.0f;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 5u);

    CHECK((vu1.state().vi[3]) == (0)); // FCGET before cycle four should observe the previous CLIP value
    CHECK((vu1.state().vi[4]) == (1)); // FCGET on cycle four should observe the committed +X CLIP bit
    CHECK((vu1.state().clip) == (1u)); // CLIP should shift and commit the six new comparison bits
}

TEST_CASE("vu1: VU1 FMAC normalizes overflow and underflow before writing results", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, 0u, makeVuUpper(0x2Au, 0xFu, 2u, 1u, 3u));

    VuInterpreter vu1;
    vu1.state().vf[1][0] = std::numeric_limits<float>::max();
    vu1.state().vf[1][1] = std::numeric_limits<float>::min();
    vu1.state().vf[1][2] = -2.0f;
    vu1.state().vf[1][3] = 0.0f;
    vu1.state().vf[2][0] = 2.0f;
    vu1.state().vf[2][1] = 0.5f;
    vu1.state().vf[2][2] = 1.0f;
    vu1.state().vf[2][3] = 1.0f;

    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 5u);

    CHECK((vu1.state().mac) ==
          (0x8425u)); // MUL should report x overflow, y underflow+zero, z sign and w zero
    CHECK((vu1.state().status) == (0x3CFu)); // current and sticky status should summarize Z/S/U/O
    CHECK((vu1.state().vf[3][0]) ==
          (std::numeric_limits<float>::max())); // overflow should clamp to the largest finite VU value
    CHECK((vu1.state().vf[3][1]) == (0.0f));    // underflow should flush to signed zero before writeback
}

TEST_CASE("vu1: FMAC product contributes Z/S/U/O sticky flags", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, 0u, makeVuUpper(0x29u, 0x8u, 2u, 1u, 3u)); // MADD.x

    VuInterpreter vu1;
    vu1.state().acc[0] = 1.0f;
    vu1.state().vf[1][0] = std::numeric_limits<float>::min();
    vu1.state().vf[2][0] = 0.5f;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 5u);

    CHECK((vu1.state().vf[3][0]) == (1.0f)); // the accumulated FMAC result should remain normal
    CHECK((vu1.state().mac) == (0u));        // MAC flags should describe the final accumulated value
    CHECK((vu1.state().status) == (0x140u)); // the underflowing product should set sticky Z and U
}

TEST_CASE("vu1: reserved opcodes stop before executing or corrupting state", "[vu1]") {
    Vu1Fixture fx;

    writeVuInstructionPair(fx.code, 0u, 0u, 0x30u);
    writeVuInstructionPair(fx.code, 8u, makeVuIaddiu(1u, 0u, 7), kVuUpperNop);

    VuInterpreter vu1;
    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.host, 0u, 0u, 0u, 8u);

    CHECK((vu1.state().cycles) ==
          (static_cast<uint64_t>(0u))); // reserved opcode should stop before consuming its issue cycle
    CHECK((vu1.state().pc) == (0u));    // reserved opcode should retain the diagnostic PC
    CHECK((vu1.state().vi[1]) == (0));  // instruction following a reserved opcode must not execute
    CHECK(fx.host.reserved == 1);       // and the host hears about it (the device stops the machine)
}
