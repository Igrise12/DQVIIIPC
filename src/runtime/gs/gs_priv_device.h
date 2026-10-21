// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/gs/priv_regs.h"
#include "runtime/machine/machine_services.h"
#include "runtime/machine/mmio.h"

#include <cstdint>
#include <string_view>

namespace dq8::gs {
class IGsSink;
}

// The GS privileged registers (physical 0x12000000, 8 KiB) and the kernel calls that own them: GsSetCrt
// (alias SetGsCrt), GsGetIMR/GsPutIMR and their interrupt forms, SetVSyncFlag and GetGsDxDyOffset. Register
// semantics follow PCSX2 (GPL-3.0-or-later, pcsx2/GS.cpp and pcsx2/Counters.cpp at 5b6e6e2): only CSR and
// SIGLBLID read back (everything else reads as CSR); CSR's SIGNAL, FINISH, HSINT, VSINT and EDWINT clear on a
// written 1 and RESET clears the page; IMR keeps bits 8-12 and reads as 0x6000 above them; unmasking a set
// cause raises INTC 0. At vblank start (gs phase) FIELD toggles in interlaced modes (it is set in progressive
// ones), VSINT is set and the page goes to the GS sink; in the kernel phase the SetVSyncFlag targets are
// written.
namespace dq8::rt {

class GsPrivDevice final : public IDevice, public IMmioDevice {
  public:
    static constexpr std::uint32_t base = 0x12000000u;
    // IMR bits.
    static constexpr std::uint64_t imr_sigmsk = 1u << 8;
    static constexpr std::uint64_t imr_finishmsk = 1u << 9;
    static constexpr std::uint64_t imr_hsmsk = 1u << 10;
    static constexpr std::uint64_t imr_vsmsk = 1u << 11;
    static constexpr std::uint64_t imr_edwmsk = 1u << 12;

    explicit GsPrivDevice(gs::IGsSink& sink) noexcept : sink_(sink) {}

    std::string_view name() const override { return "gs-priv"; }
    void attach(MachineServices& machine) override;
    void reset() override;

    bool read(std::uint32_t offset, std::uint32_t width, std::uint8_t* out, const MmioContext& at) override;
    bool write(std::uint32_t offset, std::uint32_t width, const std::uint8_t* in,
               const MmioContext& at) override;

    // The GIF's view: GS register writes it found while cutting packets (SIGNAL, FINISH, LABEL).
    void signal(std::uint64_t data);
    void finish();
    void label(std::uint64_t data);

    const gs::PrivRegs& regs() const noexcept { return regs_; }
    std::uint64_t csr() const noexcept { return regs_.read64(gs::PrivReg::csr); }
    std::uint64_t imr() const noexcept { return regs_.read64(gs::PrivReg::imr); }

  private:
    // One listener per vblank phase the device takes part in.
    class PhaseListener final : public IVblankListener {
      public:
        PhaseListener(GsPrivDevice& device, VblankPhase phase) noexcept : device_(device), phase_(phase) {}
        void on_vblank_start(std::uint64_t field) override;

      private:
        GsPrivDevice& device_;
        VblankPhase phase_;
    };

    void vblank_gs(std::uint64_t field);
    void vblank_kernel();
    void write_csr(std::uint64_t value);
    void write_imr(std::uint64_t value);
    void raise_if_unmasked(std::uint64_t csr_bit, std::uint64_t imr_bit);
    void register_services(MachineServices& machine);

    gs::IGsSink& sink_;
    MachineServices* machine_ = nullptr;
    gs::PrivRegs regs_;
    std::uint32_t vsync_flag_ = 0; // SetVSyncFlag targets (guest addresses), valid while armed_
    std::uint32_t vsync_csr_ = 0;
    bool armed_ = false;
    PhaseListener gs_phase_{*this, VblankPhase::gs};
    PhaseListener kernel_phase_{*this, VblankPhase::kernel};
};

} // namespace dq8::rt
