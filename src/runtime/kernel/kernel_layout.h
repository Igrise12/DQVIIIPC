// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>

// Guest RAM the HLE kernel uses for itself (architecture §5.1). The real EE kernel lives below the user area
// that programs load at (0x00100000); our kernel is native, so that RAM is free except for what programs copy
// into it (the SDK's Copy/SetSyscall kernel patches). The HLE takes the top 128 KiB of it: interrupt and
// alarm handlers run on a stack there (never on a stack carved from the top of RAM, where programs put their
// main thread), and the rest is reserved. A sentinel pattern fills the region at reset and is verified around
// handler invocations and at every vblank; a Copy or SetSyscall target inside it stops the guest (StopReason
// kernel_ram_collision).
namespace dq8::rt {

inline constexpr std::uint32_t hle_kernel_ram_base = 0x000E0000u;
inline constexpr std::uint32_t hle_kernel_ram_end = 0x00100000u;
// Interrupt, DMAC and alarm handlers: one stack, since EE handlers never nest.
inline constexpr std::uint32_t hle_handler_stack_base = 0x000F0000u;
inline constexpr std::uint32_t hle_handler_stack_top = 0x000FFFF0u; // initial $sp, 16-byte aligned
// The word pattern the reserved region is filled with ("HLEK" read as a little-endian word).
inline constexpr std::uint32_t hle_kernel_sentinel = 0x4B454C48u;

constexpr bool in_hle_kernel_ram(std::uint32_t physical) noexcept {
    return physical >= hle_kernel_ram_base && physical < hle_kernel_ram_end;
}

} // namespace dq8::rt
