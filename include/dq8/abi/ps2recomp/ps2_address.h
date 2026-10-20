// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// The EE address map as the game module and the host both see it. Generated code takes a fast path
// (rdram[addr & PS2_RAM_MASK]) for everything ps2_is_special_address() rejects and calls the host's
// load/store for everything it accepts. Unlike upstream PS2Recomp, the predicate covers the whole VU
// range and the FAST_* macros use it too (Docs/milestones/M2-first-recompile.md, "address routing").
#pragma once

#include <cstdint>

inline constexpr std::uint32_t PS2_RAM_SIZE = 0x02000000u; // 32 MiB main RAM
inline constexpr std::uint32_t PS2_RAM_MASK = PS2_RAM_SIZE - 1u;

inline constexpr std::uint32_t PS2_SCRATCHPAD_BASE = 0x70000000u;
inline constexpr std::uint32_t PS2_SCRATCHPAD_SIZE = 0x00004000u; // 16 KiB

// Physical ranges (after removing the kseg0/kseg1 bits). All of them lie in [0x10000000, 0x20000000).
inline constexpr std::uint32_t PS2_IO_BASE = 0x10000000u; // EE hardware registers, DMAC, timers, FIFOs
inline constexpr std::uint32_t PS2_IO_END = 0x10010000u;
inline constexpr std::uint32_t PS2_VU_BASE = 0x11000000u; // VU0/VU1 micro and data memory
inline constexpr std::uint32_t PS2_VU_END = 0x11010000u;
inline constexpr std::uint32_t PS2_GS_PRIV_BASE = 0x12000000u; // GS privileged registers
inline constexpr std::uint32_t PS2_GS_PRIV_END = 0x12002000u;
inline constexpr std::uint32_t PS2_IOP_RAM_BASE = 0x1C000000u; // IOP RAM as the EE sees it
inline constexpr std::uint32_t PS2_IOP_RAM_END = 0x1C200000u;
inline constexpr std::uint32_t PS2_BIOS_BASE = 0x1FC00000u;
inline constexpr std::uint32_t PS2_BIOS_END = 0x20000000u;

// Physical address for the direct-mapped segments: kseg0 (0x80000000) and kseg1 (0xA0000000) drop their top
// three bits; kuseg is identity-mapped the way the EE kernel sets the TLB up. kseg2/3 are not translated.
constexpr std::uint32_t ps2_physical_address(std::uint32_t vaddr) noexcept {
    return (vaddr >= 0x80000000u && vaddr < 0xC0000000u) ? (vaddr & 0x1FFFFFFFu) : vaddr;
}

constexpr bool ps2_is_scratchpad_address(std::uint32_t vaddr) noexcept {
    return vaddr >= PS2_SCRATCHPAD_BASE && vaddr < PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE;
}

// True when an access must go to the host instead of the RAM fast path: scratchpad, kseg2/kseg3 (TLB-mapped
// kernel space, including the 0xF0000000 scratchpad alias), and every physical address in
// [0x10000000, 0x20000000) (hardware registers, VU memory, GS privileged registers, IOP RAM, BIOS).
// Everything else is RAM or one of its mirrors (0x20000000 uncached, 0x30000000 uncached-accelerated), which
// the fast path's mask resolves.
constexpr bool ps2_is_special_address(std::uint32_t vaddr) noexcept {
    if (ps2_is_scratchpad_address(vaddr) || vaddr >= 0xC0000000u) {
        return true;
    }
    const std::uint32_t p = ps2_physical_address(vaddr);
    return p >= PS2_IO_BASE && p < PS2_BIOS_END;
}

static_assert(!ps2_is_special_address(0x00100008u));
static_assert(!ps2_is_special_address(0x80100008u) && !ps2_is_special_address(0xA0100008u));
static_assert(!ps2_is_special_address(0x20100000u) && !ps2_is_special_address(0x30100000u));
static_assert(ps2_is_special_address(0x70000000u) && ps2_is_special_address(0x70003FFFu));
static_assert(ps2_is_special_address(0x10003000u) && ps2_is_special_address(0xB0003000u));
static_assert(ps2_is_special_address(0x11000000u) && ps2_is_special_address(0x1100FFF0u));
static_assert(ps2_is_special_address(0x12001000u) && ps2_is_special_address(0xBC000000u));
static_assert(ps2_is_special_address(0xBFC00000u) && ps2_is_special_address(0xF0000000u));
