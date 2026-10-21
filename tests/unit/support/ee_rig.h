// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/dma/dmac.h"
#include "runtime/dma/gif.h"
#include "runtime/dma/vif.h"
#include "runtime/gs/gs_priv_device.h"
#include "runtime/gs/gs_sink.h"
#include "runtime/machine/ee_devices.h"
#include "runtime/machine/ee_timers.h"
#include "runtime/vu/vu_units.h"
#include "support/fake_machine.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

// The EE devices from make_ee_devices() on a FakeMachine, with the MMIO bus wired into guest memory and a
// GS sink that records everything, for device tests (Docs/milestones/M3-boots.md, session B).
namespace dq8::test {

struct CapturedTransfer {
    gs::GifPath path = gs::GifPath::path3;
    std::vector<std::uint8_t> bytes;
};

class CaptureSink final : public gs::IGsSink {
  public:
    void gif_transfer(gs::GifPath path, std::span<const std::uint8_t> qwords) override {
        transfers.push_back({path, {qwords.begin(), qwords.end()}});
    }
    void vsync(const gs::VsyncInfo& info) override { vsyncs.push_back(info); }
    // Answers readbacks with bytes 0, 1, 2, ... (continuing across calls).
    std::size_t read_transfer(std::span<std::uint8_t> out) override {
        for (std::uint8_t& b : out) {
            b = static_cast<std::uint8_t>(read_counter++);
        }
        ++reads;
        return out.size();
    }

    std::vector<CapturedTransfer> transfers;
    std::vector<gs::VsyncInfo> vsyncs;
    std::uint64_t read_counter = 0;
    std::uint64_t reads = 0;
};

class EeRig {
  public:
    EeRig() : EeRig(sink) {}
    explicit EeRig(gs::IGsSink& gs_sink) {
        devices = rt::make_ee_devices(rt::EeDevicesConfig{&gs_sink});
        for (auto& d : devices) {
            d->attach(machine);
        }
        for (auto& d : devices) {
            d->reset();
        }
        machine.memory().set_mmio_bus(&machine.mmio(), &machine.clock(), machine.cost().mmio_access);
    }
    EeRig(const EeRig&) = delete;
    EeRig& operator=(const EeRig&) = delete;

    template <class T> T& device(std::string_view name) {
        for (auto& d : devices) {
            if (d->name() == name) {
                return dynamic_cast<T&>(*d);
            }
        }
        throw std::logic_error("no device named " + std::string(name));
    }
    rt::Dmac& dmac() { return device<rt::Dmac>("dmac"); }
    rt::Vif& vif0() { return device<rt::Vif>("vif0"); }
    rt::Vif& vif1() { return device<rt::Vif>("vif1"); }
    rt::Gif& gif() { return device<rt::Gif>("gif"); }
    rt::VuUnits& vu() { return device<rt::VuUnits>("vu"); }
    rt::GsPrivDevice& gs() { return device<rt::GsPrivDevice>("gs-priv"); }
    rt::EeTimers& timers() { return device<rt::EeTimers>("ee-timers"); }
    rt::GuestMemory& mem() { return machine.memory(); }

    // Guest stores and loads by physical address, through the MMIO bus as generated code would.
    void w32(std::uint32_t addr, std::uint32_t value) { mem().store32(addr, value); }
    std::uint32_t r32(std::uint32_t addr) { return mem().load32(addr); }
    void w64(std::uint32_t addr, std::uint64_t value) { mem().store64(addr, value); }
    std::uint64_t r64(std::uint32_t addr) { return mem().load64(addr); }
    void w128(std::uint32_t addr, const std::uint8_t* bytes) {
        std::uint64_t lo = 0;
        std::uint64_t hi = 0;
        std::memcpy(&lo, bytes, 8);
        std::memcpy(&hi, bytes + 8, 8);
        mem().store128(addr, lo, hi);
    }

    // Runs every event due within `cycles` from now.
    void run_for(rt::Cycles cycles) { machine.run_events_until(machine.clock().now() + cycles); }

    FakeMachine machine;
    CaptureSink sink;
    std::vector<std::unique_ptr<rt::IDevice>> devices;
};

// Little-endian byte builders for test packets.
inline void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), p, p + 4);
}
inline void put64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), p, p + 8);
}

} // namespace dq8::test
