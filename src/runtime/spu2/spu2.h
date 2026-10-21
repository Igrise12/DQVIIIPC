// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/iop/hw/iop_hw.h"
#include "runtime/machine/machine_services.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// The SPU2 (architecture §7.1, ADR-0014): the sound chip the interpreted LIBSD programs through its registers
// at IOP 0x1F900000 and IOP DMA channels 4 and 7, mixing 48 kHz stereo into AudioOutput in virtual time. M4
// session B implements it; on day 0 this is a stub that attaches, schedules nothing, and stops the machine on
// the first register access or DMA, so the M3 path (where no IOP code runs) is unchanged.
namespace dq8::rt {

class AudioOutput;

struct Spu2Config {
    AudioOutput* output = nullptr; // where the mixed frames go; null mixes into nothing
};

class Spu2 final : public IDevice, public IIopBusDevice, public IIopDmaDevice, public IIopHardwareClient {
  public:
    static constexpr std::uint32_t base = 0x1F900000u;
    static constexpr std::uint32_t size = 0x800u;
    static constexpr std::uint32_t ram_bytes = 2u << 20;

    explicit Spu2(Spu2Config config);

    std::string_view name() const override { return "spu2"; }
    void attach(MachineServices& machine) override;
    void reset() override;

    bool read(std::uint32_t offset, std::uint32_t width, std::uint32_t& value, const IopAccess& at) override;
    bool write(std::uint32_t offset, std::uint32_t width, std::uint32_t value, const IopAccess& at) override;
    IopDmaStart dma_start(const IopDmaRequest& request) override;
    void dma_stop(std::uint32_t channel, Cycles at) override;
    void connect(IIopHardwareHost& host) override;

    // Mixes every sample due before `t` (session B).
    void advance_to(Cycles t);
    // The device's mapping for IopConfig::devices.
    IopDeviceMapping mapping();
    // SHA-1 of the 2 MiB sound RAM and of the register state, for determinism checks and PCSX2 comparisons.
    std::string ram_sha1() const;

  private:
    [[noreturn]] void not_implemented(const std::string& what) const;

    Spu2Config config_;
    MachineServices* machine_ = nullptr;
    IIopHardwareHost* host_ = nullptr;
};

} // namespace dq8::rt
