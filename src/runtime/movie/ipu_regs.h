// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/machine/machine_services.h"
#include "runtime/machine/mmio.h"

#include <cstdint>
#include <string_view>

// The IPU's registers as the sceMpeg HLE leaves them (architecture §7.2, ADR-0015): the movie player's own
// IPU init and reset run recompiled against this stub, while the HLE replaces everything that decodes.
// Registers at 0x10002000 (CMD, CTRL, BP, TOP) and the FIFOs at 0x10007000 (OUT, IN). Reads show an idle IPU;
// CTRL writes (reset) and the set-up commands BCLR, SETIQ, SETVQ and SETTH are accepted; IN FIFO writes are
// discarded and counted; a decode command (IDEC, BDEC, VDEC, FDEC, CSC, PACK) or an OUT FIFO read stops the
// machine, since the HLE should have covered that path. Day-0 stub; M4 session C extends it (SETTH capture
// for the CSC alpha).
namespace dq8::rt {

class IpuRegisters final : public IDevice, public IMmioDevice {
  public:
    static constexpr std::uint32_t regs_base = 0x10002000u;
    static constexpr std::uint32_t fifo_base = 0x10007000u;

    std::string_view name() const override { return "ipu"; }
    void attach(MachineServices& machine) override;
    void reset() override;
    bool read(std::uint32_t offset, std::uint32_t width, std::uint8_t* out, const MmioContext& at) override;
    bool write(std::uint32_t offset, std::uint32_t width, const std::uint8_t* in,
               const MmioContext& at) override;

    std::uint64_t commands() const noexcept { return commands_; }
    std::uint64_t fifo_writes() const noexcept { return fifo_writes_; }

  private:
    // The FIFO page, mapped separately (its offsets overlap the register page's).
    class Fifo final : public IMmioDevice {
      public:
        explicit Fifo(IpuRegisters& owner) : owner_(owner) {}
        std::string_view name() const override { return "ipu.fifo"; }
        bool read(std::uint32_t offset, std::uint32_t width, std::uint8_t* out,
                  const MmioContext& at) override;
        bool write(std::uint32_t offset, std::uint32_t width, const std::uint8_t* in,
                   const MmioContext& at) override;

      private:
        IpuRegisters& owner_;
    };

    [[noreturn]] void decode_path(std::string_view what, std::uint32_t value, const MmioContext& at) const;

    Fifo fifo_{*this};
    MachineServices* machine_ = nullptr;
    std::uint64_t commands_ = 0;
    std::uint64_t fifo_writes_ = 0;
};

} // namespace dq8::rt
