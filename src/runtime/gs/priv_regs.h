// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// The GS privileged registers (physical 0x12000000-0x12001FFF) as an 8 KiB byte image: the layout
// paraLLEl-GS's GSInterface::get_priv_register_state() uses (Docs/spikes/S5-parallel-gs-standalone.md), so
// the image is handed to a backend unchanged at each vsync. Register offsets and CSR bits are the public GS
// hardware layout.
namespace dq8::gs {

inline constexpr std::size_t priv_page_size = 0x2000;

enum class PrivReg : std::uint32_t {
    pmode = 0x0000,
    smode1 = 0x0010,
    smode2 = 0x0020,
    srfsh = 0x0030,
    synch1 = 0x0040,
    synch2 = 0x0050,
    syncv = 0x0060,
    dispfb1 = 0x0070,
    display1 = 0x0080,
    dispfb2 = 0x0090,
    display2 = 0x00A0,
    extbuf = 0x00B0,
    extdata = 0x00C0,
    extwrite = 0x00D0,
    bgcolor = 0x00E0,
    csr = 0x1000,
    imr = 0x1010,
    busdir = 0x1040,
    siglblid = 0x1080,
};

// CSR bits.
inline constexpr std::uint64_t csr_signal = 1u << 0;
inline constexpr std::uint64_t csr_finish = 1u << 1;
inline constexpr std::uint64_t csr_hsint = 1u << 2;
inline constexpr std::uint64_t csr_vsint = 1u << 3;
inline constexpr std::uint64_t csr_edwint = 1u << 4;
inline constexpr std::uint64_t csr_flush = 1u << 8;
inline constexpr std::uint64_t csr_reset = 1u << 9;
inline constexpr std::uint64_t csr_nfield = 1u << 12;
inline constexpr std::uint64_t csr_field = 1u << 13;

struct PrivRegs {
    std::array<std::uint8_t, priv_page_size> page{};

    std::uint64_t read64(PrivReg reg) const noexcept {
        std::uint64_t v = 0;
        std::memcpy(&v, page.data() + static_cast<std::uint32_t>(reg), sizeof v);
        return v;
    }
    void write64(PrivReg reg, std::uint64_t value) noexcept {
        std::memcpy(page.data() + static_cast<std::uint32_t>(reg), &value, sizeof value);
    }
};

} // namespace dq8::gs
