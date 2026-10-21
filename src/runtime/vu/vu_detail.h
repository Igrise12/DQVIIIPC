// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xRuntime/src/lib/vu/ps2_vu1_detail.h.
#pragma once

#include <cstdint>

// Instruction field helpers shared by the interpreter translation units (internal).
namespace dq8::rt::vu_detail {

// Instruction field extraction helpers
inline uint8_t DEST(uint32_t i) {
    return static_cast<uint8_t>((i >> 21) & 0xF);
}
inline uint8_t FT(uint32_t i) {
    return static_cast<uint8_t>((i >> 16) & 0x1F);
}
inline uint8_t FS(uint32_t i) {
    return static_cast<uint8_t>((i >> 11) & 0x1F);
}
inline uint8_t FD(uint32_t i) {
    return static_cast<uint8_t>((i >> 6) & 0x1F);
}
inline uint8_t BC(uint32_t i) {
    return static_cast<uint8_t>(i & 0x3);
}

// Lower instruction field helpers
inline uint8_t LIT(uint32_t i) {
    return static_cast<uint8_t>((i >> 16) & 0x1F);
}
inline uint8_t LIS(uint32_t i) {
    return static_cast<uint8_t>((i >> 11) & 0x1F);
}
inline uint8_t LID(uint32_t i) {
    return static_cast<uint8_t>((i >> 6) & 0x1F);
}
inline uint8_t VIT(uint32_t i) {
    return static_cast<uint8_t>((i >> 16) & 0xF);
}
inline uint8_t VIS(uint32_t i) {
    return static_cast<uint8_t>((i >> 11) & 0xF);
}
inline uint8_t VID(uint32_t i) {
    return static_cast<uint8_t>((i >> 6) & 0xF);
}
inline int16_t IMM11(uint32_t i) {
    return static_cast<int16_t>(static_cast<int32_t>(i << 21) >> 21);
}
inline int16_t IMM15(uint32_t i) {
    uint32_t lo11 = i & 0x7FF;
    uint32_t hi4 = (i >> 21) & 0xF;
    uint32_t raw = (hi4 << 11) | lo11;
    return static_cast<int16_t>(static_cast<int32_t>(raw << 17) >> 17);
}

} // namespace dq8::rt::vu_detail
