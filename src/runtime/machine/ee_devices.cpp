// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/machine/ee_devices.h"

#include "runtime/dma/dmac.h"
#include "runtime/dma/gif.h"
#include "runtime/dma/register_block.h"
#include "runtime/dma/vif.h"
#include "runtime/gs/gs_priv_device.h"
#include "runtime/machine/ee_timers.h"
#include "runtime/machine/intc_regs.h"
#include "runtime/movie/ipu_regs.h"
#include "runtime/vu/vu_units.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace dq8::rt {

namespace {

// The EE's serial port as a TTY (0x1000F100-0x1000F1FF): bytes written to the TX FIFO (0x1000F180) are
// collected into lines and emitted as "tty" events; the control and status registers store what is
// written and read back as written (an idle, never-full port); the RX FIFO reads 0.
class SioTty final : public IDevice, public IRegisterBlock {
  public:
    static constexpr std::uint32_t base = 0x1000F100u;
    static constexpr std::uint32_t tx_fifo = 0x80u;
    static constexpr std::uint32_t rx_fifo = 0xC0u;
    static constexpr std::size_t max_line = 1024;

    std::string_view name() const override { return "sio-tty"; }

    void attach(MachineServices& machine) override {
        machine_ = &machine;
        window_.emplace(*this, 0u);
        machine.mmio().map(base, 0x100u, *window_);
    }

    void reset() override {
        regs_ = {};
        line_.clear();
    }

    bool read32(std::uint32_t offset, std::uint32_t& value, const MmioContext& /*at*/) override {
        if (offset == rx_fifo || offset == tx_fifo) {
            value = 0;
            return true;
        }
        if (offset % 0x10 != 0 || offset > 0x50) {
            return false;
        }
        value = regs_[offset / 0x10];
        return true;
    }

    bool write32(std::uint32_t offset, std::uint32_t value, std::uint32_t mask,
                 const MmioContext& /*at*/) override {
        if (offset == tx_fifo) {
            put(static_cast<char>(value & 0xFFu));
            return true;
        }
        if (offset == rx_fifo) {
            return true;
        }
        if (offset % 0x10 != 0 || offset > 0x50) {
            return false;
        }
        regs_[offset / 0x10] = merge_masked(regs_[offset / 0x10], value, mask);
        return true;
    }

  private:
    void put(char c) {
        if (c == '\r') {
            return;
        }
        if (c != '\n') {
            line_.push_back(c);
            if (line_.size() < max_line) {
                return;
            }
        }
        machine_->emit(MachineEvent{"tty", {}}.add("text", line_));
        line_.clear();
    }

    MachineServices* machine_ = nullptr;
    std::optional<RegisterWindow> window_;
    std::array<std::uint32_t, 6> regs_{}; // LCR, LSR, IER, ISR, FCR, BGR
    std::string line_;
};

} // namespace

std::vector<std::unique_ptr<IDevice>> make_ee_devices(const EeDevicesConfig& config) {
    if (config.gs == nullptr) {
        throw std::invalid_argument("make_ee_devices: EeDevicesConfig::gs is required");
    }
    auto timers = std::make_unique<EeTimers>();
    auto intc = std::make_unique<IntcRegs>();
    auto gs_priv = std::make_unique<GsPrivDevice>(*config.gs);
    auto gif = std::make_unique<Gif>(*config.gs, *gs_priv);
    auto vu = std::make_unique<VuUnits>(*gif);
    auto vif0 = std::make_unique<Vif>(0, *vu, nullptr);
    auto vif1 = std::make_unique<Vif>(1, *vu, gif.get());
    auto dmac = std::make_unique<Dmac>(*vif0, *vif1, *gif);
    auto sio = std::make_unique<SioTty>();
    auto ipu = std::make_unique<IpuRegisters>(); // M4: the IPU stub under the sceMpeg HLE (session C)

    std::vector<std::unique_ptr<IDevice>> devices;
    devices.push_back(std::move(timers));
    devices.push_back(std::move(intc));
    devices.push_back(std::move(gs_priv));
    devices.push_back(std::move(gif));
    devices.push_back(std::move(vu));
    devices.push_back(std::move(vif0));
    devices.push_back(std::move(vif1));
    devices.push_back(std::move(dmac));
    devices.push_back(std::move(ipu));
    devices.push_back(std::move(sio));
    return devices;
}

} // namespace dq8::rt
