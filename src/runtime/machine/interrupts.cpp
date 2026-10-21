// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/machine/interrupts.h"

namespace dq8::rt {

namespace {

constexpr std::uint32_t intc_bits = (1u << intc_line_count) - 1u;
// D_STAT status bits that writes may clear, and mask bits that writes may toggle.
constexpr std::uint32_t dstat_status_bits = 0x3FFu | InterruptController::dstat_sis |
                                            InterruptController::dstat_meis | InterruptController::dstat_beis;
constexpr std::uint32_t dstat_mask_bits =
    (0x3FFu << 16) | InterruptController::dstat_sim | InterruptController::dstat_meim;

} // namespace

std::string_view to_string(IntcLine line) noexcept {
    switch (line) {
    case IntcLine::gs:
        return "gs";
    case IntcLine::sbus:
        return "sbus";
    case IntcLine::vblank_start:
        return "vblank-start";
    case IntcLine::vblank_end:
        return "vblank-end";
    case IntcLine::vif0:
        return "vif0";
    case IntcLine::vif1:
        return "vif1";
    case IntcLine::vu0:
        return "vu0";
    case IntcLine::vu1:
        return "vu1";
    case IntcLine::ipu:
        return "ipu";
    case IntcLine::timer0:
        return "timer0";
    case IntcLine::timer1:
        return "timer1";
    case IntcLine::timer2:
        return "timer2";
    case IntcLine::timer3:
        return "timer3";
    case IntcLine::sfifo:
        return "sfifo";
    case IntcLine::vu0_watchdog:
        return "vu0-watchdog";
    }
    return "unknown";
}

std::string_view to_string(DmaChannel channel) noexcept {
    switch (channel) {
    case DmaChannel::vif0:
        return "vif0";
    case DmaChannel::vif1:
        return "vif1";
    case DmaChannel::gif:
        return "gif";
    case DmaChannel::ipu_from:
        return "ipu-from";
    case DmaChannel::ipu_to:
        return "ipu-to";
    case DmaChannel::sif0:
        return "sif0";
    case DmaChannel::sif1:
        return "sif1";
    case DmaChannel::sif2:
        return "sif2";
    case DmaChannel::spr_from:
        return "spr-from";
    case DmaChannel::spr_to:
        return "spr-to";
    }
    return "unknown";
}

void InterruptController::raise_intc(IntcLine line) {
    intc_stat_ |= 1u << static_cast<unsigned>(line);
    notify();
}

void InterruptController::write_intc_stat(std::uint32_t value) noexcept {
    intc_stat_ &= ~(value & intc_bits);
}

void InterruptController::write_intc_mask(std::uint32_t value) {
    intc_mask_ ^= value & intc_bits;
    notify(); // unmasking a set cause makes it pending
}

void InterruptController::raise_dmac(DmaChannel channel) {
    dstat_ |= 1u << static_cast<unsigned>(channel);
    notify();
}

void InterruptController::raise_dmac_status(std::uint32_t status_bits) {
    dstat_ |= status_bits & (dstat_sis | dstat_meis | dstat_beis);
    notify();
}

void InterruptController::write_dmac_stat(std::uint32_t value) {
    dstat_ &= ~(value & dstat_status_bits);
    dstat_ ^= value & dstat_mask_bits;
    notify();
}

bool InterruptController::int1_pending() const noexcept {
    return dmac_pending_channels() != 0 || ((dstat_ & dstat_sis) != 0 && (dstat_ & dstat_sim) != 0) ||
           ((dstat_ & dstat_meis) != 0 && (dstat_ & dstat_meim) != 0) || (dstat_ & dstat_beis) != 0;
}

void InterruptController::reset() noexcept {
    intc_stat_ = 0;
    intc_mask_ = 0;
    dstat_ = 0;
}

void InterruptController::notify() {
    if (hook_ && pending()) {
        hook_();
    }
}

} // namespace dq8::rt
