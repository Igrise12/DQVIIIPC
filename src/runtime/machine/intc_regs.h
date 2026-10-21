// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/dma/register_block.h"
#include "runtime/machine/machine_services.h"

#include <cstdint>
#include <optional>
#include <string_view>

// The INTC registers (I_STAT at 0x1000F000, I_MASK at 0x1000F010) as a device: accesses are forwarded to the
// machine's InterruptController, which holds the bits and their write semantics. The device also raises the
// vblank causes in the intc phase (INTC 2 at vblank start, INTC 3 at vblank end), after the GS, kernel and
// IOP phases have run (Docs/milestones/M3-boots.md, "Vblank phases").
namespace dq8::rt {

class IntcRegs final : public IDevice, public IRegisterBlock, public IVblankListener {
  public:
    static constexpr std::uint32_t base = 0x1000F000u;
    static constexpr std::uint32_t i_stat = 0x00u;
    static constexpr std::uint32_t i_mask = 0x10u;

    std::string_view name() const override { return "intc"; }
    void attach(MachineServices& machine) override;
    void reset() override;

    bool read32(std::uint32_t offset, std::uint32_t& value, const MmioContext& at) override;
    bool write32(std::uint32_t offset, std::uint32_t value, std::uint32_t mask,
                 const MmioContext& at) override;

    void on_vblank_start(std::uint64_t field) override;
    void on_vblank_end(std::uint64_t field) override;

  private:
    MachineServices* machine_ = nullptr;
    std::optional<RegisterWindow> window_;
};

} // namespace dq8::rt
