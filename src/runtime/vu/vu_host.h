// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <span>

// What a VU interpreter needs from the machine (replaces upstream PS2Recomp's GS& and PS2Memory*): somewhere
// to send XGKICK packets (PATH1 of the GIF), a counter that changes whenever micro memory changes so decoded
// instructions can be cached, and a place to report a reserved instruction (the machine stops on it).
namespace dq8::rt {

class IVuHost {
  public:
    virtual ~IVuHost() = default;
    // A whole GIF packet (tags up to EOP) taken from VU1 data memory by XGKICK.
    virtual void xgkick(std::span<const std::uint8_t> packet) = 0;
    // Changes whenever the unit's micro memory may have changed.
    virtual std::uint64_t code_generation() const = 0;
    // The interpreter stopped before executing a reserved upper or lower instruction at `pc`.
    virtual void reserved_instruction(bool upper, std::uint32_t pc, std::uint32_t instruction) = 0;
};

} // namespace dq8::rt
