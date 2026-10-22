// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/iop/hw/iop_hw.h"
#include "runtime/machine/events.h"
#include "runtime/machine/machine_services.h"
#include "runtime/platform/audio.h"
#include "runtime/spu2/spu2_chip.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The SPU2 (architecture §7.1, ADR-0014): the sound chip the interpreted LIBSD programs through its registers
// at IOP 0x1F900000 and IOP DMA channels 4 (core 0) and 7 (core 1), mixing 48 kHz stereo into AudioOutput in
// virtual time. spu2::Chip (spu2_chip.h) is the chip; this device clocks it and connects it to the IOP.
//
// Time: sample n of the chip spans EE cycles [n, n+1) * ee_cycles_per_spu2_sample and is mixed once that span
// has passed. Before the first register write or DMA start after reset the chip is idle and schedules
// nothing (the M3 path is unchanged); at that first use the samples already past are output as silence, so
// that output frame n is always sample n of the run. advance_to(t) mixes every sample that ends at or
// before t and runs every DMA step due by t, samples first at equal times (PCSX2's TimeUpdate); every
// register access and DMA start calls it first. One event drives the chip: at most cost.spu2_block_samples
// samples ahead, sooner when an IRQ or an AutoDMA completion may come earlier, and at each manual DMA step,
// so that IRQs and completions are delivered at their exact time. Output and state are the same however
// advance_to calls are split. Nothing flows back from AudioOutput into the chip.
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
    ~Spu2() override;
    Spu2(const Spu2&) = delete;
    Spu2& operator=(const Spu2&) = delete;

    std::string_view name() const override { return "spu2"; }
    void attach(MachineServices& machine) override;
    void reset() override;

    bool read(std::uint32_t offset, std::uint32_t width, std::uint32_t& value, const IopAccess& at) override;
    bool write(std::uint32_t offset, std::uint32_t width, std::uint32_t value, const IopAccess& at) override;
    IopDmaStart dma_start(const IopDmaRequest& request) override;
    void dma_stop(std::uint32_t channel, Cycles at) override;
    void connect(IIopHardwareHost& host) override;

    // Mixes every sample due by `t` (a no-op before first use).
    void advance_to(Cycles t);
    // The device's mapping for IopConfig::devices.
    IopDeviceMapping mapping();
    // SHA-1 of the 2 MiB sound RAM (little-endian words), for determinism checks and PCSX2 comparisons.
    std::string ram_sha1() const;
    // SHA-1 of every other piece of state that shapes future output: registers, voices, the clock.
    std::string state_sha1() const;
    // The sound RAM as little-endian bytes (--dump-spu2-at).
    std::vector<std::uint8_t> ram_bytes_le() const;

    const spu2::Chip& chip() const noexcept { return chip_; }
    bool started() const noexcept { return started_; }
    // Samples mixed since cycle 0 (the idle samples before first use included).
    std::uint64_t samples() const noexcept { return mixed_; }
    // Accesses that arrived with a time before what the chip had already processed (the IOP ran behind an
    // SPU2 event); they apply at the chip's time. Zero when the IOP keeps its time contract.
    std::uint64_t late_accesses() const noexcept { return late_accesses_; }

  private:
    struct DmaTimer {
        bool busy = false;     // a transfer is running on the channel
        bool deferred = false; // AutoDMA: completes when the chip says so
        Cycles next = 0;       // manual: time of the next step
    };

    void start(Cycles at);
    void catch_up(Cycles t);
    void mix_one();
    void dma_step(std::uint32_t core);
    void deliver(Cycles at);
    void flush();
    // (Re)schedules the event: `full` predicts IRQs from the voices; otherwise a sample ahead when any IRQ is
    // armed (register writes come in bursts; the next event predicts properly).
    void reschedule(bool full);
    void on_event(Cycles when);
    void note_time(Cycles at);
    [[noreturn]] void stop_unsupported(const std::string& what, std::uint32_t pc) const;
    void trace(std::string message) const;

    Spu2Config config_;
    MachineServices* machine_ = nullptr;
    IIopHardwareHost* host_ = nullptr;
    spu2::Chip chip_;
    bool started_ = false;
    std::uint64_t mixed_ = 0;
    Cycles processed_ = 0; // the latest time catch_up reached
    EventId event_ = 0;
    Cycles event_at_ = 0;
    std::array<DmaTimer, 2> dma_{};
    std::vector<StereoFrame> pending_;
    std::uint64_t late_accesses_ = 0;
};

} // namespace dq8::rt
