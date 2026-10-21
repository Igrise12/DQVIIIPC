// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

// The EE's two interrupt sources as the kernel sees them (architecture §5.5, §12.3): the INTC (I_STAT and
// I_MASK at 0x1000F000 and 0x1000F010, wired to COP0 INT0) and the DMAC status (D_STAT at 0x1000E010, wired
// to INT1). This class holds the bits and their write semantics; the MMIO devices forward register accesses
// to it, devices raise causes, and the scheduler delivers pending interrupts to the kernel's handler chains
// at the next safe point (handler lists and delivery live with the scheduler).
namespace dq8::rt {

// INTC cause lines (bit numbers of I_STAT / I_MASK).
enum class IntcLine : unsigned {
    gs = 0,
    sbus = 1,
    vblank_start = 2,
    vblank_end = 3,
    vif0 = 4,
    vif1 = 5,
    vu0 = 6,
    vu1 = 7,
    ipu = 8,
    timer0 = 9,
    timer1 = 10,
    timer2 = 11,
    timer3 = 12,
    sfifo = 13,
    vu0_watchdog = 14,
};
inline constexpr unsigned intc_line_count = 15;

// DMAC channels (bit numbers of D_STAT's CIS and CIM fields).
enum class DmaChannel : unsigned {
    vif0 = 0,
    vif1 = 1,
    gif = 2,
    ipu_from = 3,
    ipu_to = 4,
    sif0 = 5, // IOP -> EE
    sif1 = 6, // EE -> IOP
    sif2 = 7,
    spr_from = 8,
    spr_to = 9,
};
inline constexpr unsigned dma_channel_count = 10;

std::string_view to_string(IntcLine line) noexcept;
std::string_view to_string(DmaChannel channel) noexcept;

class InterruptController {
  public:
    // D_STAT bits beyond the channel fields.
    static constexpr std::uint32_t dstat_sis = 1u << 13;  // stall interrupt status
    static constexpr std::uint32_t dstat_meis = 1u << 14; // MFIFO empty interrupt status
    static constexpr std::uint32_t dstat_beis = 1u << 15; // bus error interrupt status
    static constexpr std::uint32_t dstat_sim = 1u << 29;  // stall interrupt mask
    static constexpr std::uint32_t dstat_meim = 1u << 30; // MFIFO empty interrupt mask

    // Called after a raise that leaves an interrupt pending (the machine forces a safe point with it).
    using RaiseHook = std::function<void()>;
    void set_raise_hook(RaiseHook hook) { hook_ = std::move(hook); }

    // INTC.
    void raise_intc(IntcLine line);
    std::uint32_t intc_stat() const noexcept { return intc_stat_; }
    std::uint32_t intc_mask() const noexcept { return intc_mask_; }
    void write_intc_stat(std::uint32_t value) noexcept; // a 1 bit clears that cause
    void write_intc_mask(std::uint32_t value);          // a 1 bit toggles that mask bit
    // Causes that are both set and unmasked: what INT0 delivers.
    std::uint32_t intc_pending() const noexcept { return intc_stat_ & intc_mask_; }
    bool int0_pending() const noexcept { return intc_pending() != 0; }

    // DMAC.
    void raise_dmac(DmaChannel channel);
    void raise_dmac_status(std::uint32_t status_bits); // SIS, MEIS or BEIS
    std::uint32_t dmac_stat() const noexcept { return dstat_; }
    // Bits 0-15 set to 1 clear status bits; bits 16-31 set to 1 toggle mask bits (as D_STAT writes do).
    void write_dmac_stat(std::uint32_t value);
    // Channels whose CIS and CIM are both set: what INT1 delivers.
    std::uint32_t dmac_pending_channels() const noexcept {
        return (dstat_ & 0x3FFu) & ((dstat_ >> 16) & 0x3FFu);
    }
    bool int1_pending() const noexcept;

    bool pending() const noexcept { return int0_pending() || int1_pending(); }

    // Power-on: everything clear.
    void reset() noexcept;

  private:
    void notify();

    std::uint32_t intc_stat_ = 0;
    std::uint32_t intc_mask_ = 0;
    std::uint32_t dstat_ = 0;
    RaiseHook hook_;
};

} // namespace dq8::rt
