// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/iop_exec.h"
#include "runtime/iop/kernel/hle.h"

// intrman as HLE (Docs/milestones/M4-playable.md, session A). Interrupt numbers 0-31 are the INTC lines;
// 0x20-0x26 and 0x28-0x2C are DMA channels 0-6 and 7-12, demultiplexed from INTC line 3 by the dispatcher
// (IopExec). A handler runs in the interrupt context as handler(arg) with the gp of the code that registered
// it; one that returns 0 has its interrupt disabled.
namespace dq8::rt {

namespace {

constexpr std::uint32_t irq_count = 0x2E;
constexpr std::uint32_t irq_dma_base = 0x20;
constexpr std::uint32_t irq_dma2_base = 0x28;
constexpr std::uint32_t dma_line = 3;

// The DMA channel of a DMA interrupt number, or -1.
int dma_channel(std::uint32_t irq) {
    if (irq >= irq_dma_base && irq < irq_dma_base + 7) {
        return static_cast<int>(irq - irq_dma_base);
    }
    if (irq >= irq_dma2_base && irq < irq_dma2_base + 6) {
        return static_cast<int>(7 + irq - irq_dma2_base);
    }
    return -1;
}

bool valid(std::uint32_t irq) {
    return irq < 32 || dma_channel(irq) >= 0;
}

void register_intr_handler(HleCall& c) {
    const std::uint32_t irq = c.arg(0);
    if (irq >= irq_count || !valid(irq)) {
        c.ret(iop_error::illegal_intrcode);
        return;
    }
    auto& handlers = c.exec().intr_handlers();
    if (handlers.count(irq) != 0) {
        c.ret(iop_error::found_handler);
        return;
    }
    handlers[irq] = IopIntrHandler{c.arg(1), c.arg(2), c.arg(3), c.caller_gp()};
    c.ret(0);
}

void release_intr_handler(HleCall& c) {
    c.ret(c.exec().intr_handlers().erase(c.arg(0)) != 0 ? iop_error::ok : iop_error::notfound_handler);
}

void enable_intr(HleCall& c) {
    const std::uint32_t irq = c.arg(0);
    if (!valid(irq)) {
        c.ret(iop_error::illegal_intrcode);
        return;
    }
    IopHardware& hw = c.exec().hardware();
    if (irq < 32) {
        hw.set_i_mask(hw.i_mask() | (1u << irq));
    } else {
        hw.set_dma_interrupt(static_cast<std::uint32_t>(dma_channel(irq)), true);
        hw.set_i_mask(hw.i_mask() | (1u << dma_line));
    }
    c.ret(0);
}

void disable_intr(HleCall& c) {
    const std::uint32_t irq = c.arg(0);
    const std::uint32_t result = c.arg(1);
    if (!valid(irq)) {
        c.ret(iop_error::illegal_intrcode);
        return;
    }
    IopHardware& hw = c.exec().hardware();
    bool was = false;
    if (irq < 32) {
        was = (hw.i_mask() & (1u << irq)) != 0;
        hw.set_i_mask(hw.i_mask() & ~(1u << irq));
    } else {
        const auto ch = static_cast<std::uint32_t>(dma_channel(irq));
        was = hw.dma_interrupt_enabled(ch);
        hw.set_dma_interrupt(ch, false);
    }
    if (!was) {
        c.ret(iop_error::intr_disable);
        return;
    }
    if (result != 0) {
        c.write32(result, irq);
    }
    c.ret(0);
}

void cpu_suspend_intr(HleCall& c) {
    const std::uint32_t state = c.arg(0);
    if (c.in_interrupt()) {
        if (state != 0) {
            c.write32(state, 0);
        }
        c.ret(iop_error::cpudi);
        return;
    }
    IopThread& t = *c.thread();
    const bool was = t.interrupts;
    t.interrupts = false;
    if (state != 0) {
        c.write32(state, was ? 1 : 0);
    }
    c.ret(was ? iop_error::ok : iop_error::cpudi);
}

void cpu_resume_intr(HleCall& c) {
    if (!c.in_interrupt()) {
        c.thread()->interrupts = (c.arg(0) & 1u) != 0;
    }
    c.ret(0);
}

void query_intr_context(HleCall& c) {
    c.ret(c.in_interrupt() ? 1u : 0u);
}

constexpr HleFunction intrman_table[] = {
    {"intrman", 4, "RegisterIntrHandler", register_intr_handler},
    {"intrman", 5, "ReleaseIntrHandler", release_intr_handler},
    {"intrman", 6, "EnableIntr", enable_intr},
    {"intrman", 7, "DisableIntr", disable_intr},
    {"intrman", 17, "CpuSuspendIntr", cpu_suspend_intr},
    {"intrman", 18, "CpuResumeIntr", cpu_resume_intr},
    {"intrman", 23, "QueryIntrContext", query_intr_context},
};

} // namespace

std::span<const HleFunction> hle_intrman() {
    return intrman_table;
}

} // namespace dq8::rt
