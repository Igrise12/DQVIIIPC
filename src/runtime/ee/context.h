// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/abi/ps2recomp/r5900_context.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// The EE register context at hand-off and its dump. R5900Context itself is part of the module ABI
// (include/dq8/abi/ps2recomp/r5900_context.h); this file only reads and writes it.
namespace dq8::rt {

// The stack pointer the program starts with: the top of 32 MiB RAM, 16-byte aligned with a little headroom,
// where the EE kernel's hand-off leaves it before crt0 calls SetupThread.
inline constexpr std::uint32_t initial_stack_pointer = 0x01FFFFF0u;

// Conventional MIPS names of the 32 GPRs, index = register number.
extern const std::array<std::string_view, 32> gpr_names;

// Resets every field to the power-on state of R5900Context, then pc = entry, sp = initial_stack_pointer and
// a0 = a1 = ra = 0 (an argc/argv of zero, and a return address that ends the program).
void reset_context(R5900Context& ctx, std::uint32_t entry);

// The low and high 64 bits of a 128-bit GPR.
std::uint64_t gpr_lo(const R5900Context& ctx, unsigned reg) noexcept;
std::uint64_t gpr_hi(const R5900Context& ctx, unsigned reg) noexcept;
std::uint32_t gpr_u32(const R5900Context& ctx, unsigned reg) noexcept;
// Sets the low 64 bits (the high 64 bits are kept), as the recompiler's SET_GPR_U64 does.
void set_gpr_lo(R5900Context& ctx, unsigned reg, std::uint64_t value) noexcept;

// A multi-line dump: pc, the 32 GPRs (all 128 bits when the upper half is non-zero), HI/LO, SA and the COP0
// status. Lines end in '\n'.
std::string format_context(const R5900Context& ctx);

} // namespace dq8::rt
