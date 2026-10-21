// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/iop/hw/iop_hw.h"
#include "runtime/iop/iop_memory.h"
#include "support/fake_machine.h"

#include <cstdint>
#include <utility>
#include <vector>

// An IIopHardwareHost for IOP-side device tests (M4 day 0; sessions A and B): IOP RAM, a settable IOP time,
// and a record of every interrupt raised and DMA completed, with times. No IOP interpreter runs.
namespace dq8::test {

class FakeIopHost final : public rt::IIopHardwareHost {
  public:
    explicit FakeIopHost(FakeMachine& machine) : machine_(machine) {}

    rt::IopMemory& ram() override { return ram_; }
    rt::MachineServices& machine() override { return machine_; }
    rt::Cycles iop_now() const override { return now; }
    void raise_irq(rt::IopIrq line, rt::Cycles at) override { irqs.emplace_back(line, at); }
    void dma_complete(std::uint32_t channel, rt::Cycles at) override { dma_done.emplace_back(channel, at); }

    rt::Cycles now = 0;
    std::vector<std::pair<rt::IopIrq, rt::Cycles>> irqs;
    std::vector<std::pair<std::uint32_t, rt::Cycles>> dma_done;

  private:
    FakeMachine& machine_;
    rt::IopMemory ram_;
};

} // namespace dq8::test
