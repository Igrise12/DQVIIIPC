// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/machine/intc_regs.h"

#include "runtime/machine/interrupts.h"

#include <cstdint>

namespace dq8::rt {

void IntcRegs::attach(MachineServices& machine) {
    machine_ = &machine;
    window_.emplace(*this, 0u);
    machine.mmio().map(base, 0x20u, *window_);
    machine.add_vblank_listener(VblankPhase::intc, *this);
}

void IntcRegs::reset() {
    machine_->interrupts().reset();
}

bool IntcRegs::read32(std::uint32_t offset, std::uint32_t& value, const MmioContext& at) {
    switch (offset) {
    case i_stat:
        value = machine_->interrupts().intc_stat();
        break;
    case i_mask:
        value = machine_->interrupts().intc_mask();
        break;
    default:
        return false;
    }
    machine_->note_poll(at.pc, base + offset, value);
    return true;
}

bool IntcRegs::write32(std::uint32_t offset, std::uint32_t value, std::uint32_t /*mask*/,
                       const MmioContext& /*at*/) {
    // Both registers act on the 1 bits only (clear / toggle), so a partial store needs no merge.
    switch (offset) {
    case i_stat:
        machine_->interrupts().write_intc_stat(value);
        break;
    case i_mask:
        machine_->interrupts().write_intc_mask(value);
        break;
    default:
        return false;
    }
    machine_->note_activity();
    return true;
}

void IntcRegs::on_vblank_start(std::uint64_t /*field*/) {
    machine_->interrupts().raise_intc(IntcLine::vblank_start);
}

void IntcRegs::on_vblank_end(std::uint64_t /*field*/) {
    machine_->interrupts().raise_intc(IntcLine::vblank_end);
}

} // namespace dq8::rt
