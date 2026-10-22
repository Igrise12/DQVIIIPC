// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/iop/cpu/iop_cpu.h"
#include "runtime/iop/hw/iop_hw.h"
#include "runtime/machine/events.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The IOP's physical address space as the interpreted modules see it (Docs/milestones/M4-playable.md, session
// A): RAM and its mirrors (read by the core directly), the 1 KiB scratchpad, the interrupt controller, the
// DMA controller, the SSBUS registers as plain stores, the devices of IopConfig::devices (the SPU2) and the
// HLE page. Every other address is unmapped: the access stops the guest with its address and pc. The IOP's
// own timers, SIO2, CDVD and the BIOS are not here: the sound modules reach none of them (timrman is HLE).
namespace dq8::rt {

class IopMemory;
class MachineServices;

// IOP physical addresses.
namespace iop_map {
inline constexpr std::uint32_t scratchpad = 0x1F800000u;
inline constexpr std::uint32_t scratchpad_size = 0x400u;
inline constexpr std::uint32_t ssbus_a = 0x1F801000u; // spd/pio address and the delay registers
inline constexpr std::uint32_t ssbus_b = 0x1F801400u; // dev1/spu2 address and delay registers
inline constexpr std::uint32_t ssbus_words = 9;
inline constexpr std::uint32_t i_stat = 0x1F801070u;
inline constexpr std::uint32_t i_mask = 0x1F801074u;
inline constexpr std::uint32_t i_ctrl = 0x1F801078u;
inline constexpr std::uint32_t dma_low = 0x1F801080u;  // channels 0-6, 0x10 apart: MADR, BCR, CHCR, TADR
inline constexpr std::uint32_t dma_high = 0x1F801500u; // channels 7-13
inline constexpr std::uint32_t dpcr = 0x1F8010F0u;
inline constexpr std::uint32_t dicr = 0x1F8010F4u;
inline constexpr std::uint32_t dpcr2 = 0x1F801570u;
inline constexpr std::uint32_t dicr2 = 0x1F801574u;
inline constexpr std::uint32_t dmacen = 0x1F801578u;
inline constexpr std::uint32_t dmacinten = 0x1F80157Cu;
// The HLE page: in the first 256 MiB (so a `j` from module code reaches it) and above every RAM mirror. Slot
// n is the 8 bytes at hle_page + 8n: the trap word iop_hle_trap_opcode | n and a zero word, as loads read
// them; an instruction fetch at a slot is the trap.
inline constexpr std::uint32_t hle_page = 0x0FF00000u;
inline constexpr std::uint32_t hle_page_size = 0x00010000u;
inline constexpr std::uint32_t hle_slot_size = 8;
inline constexpr std::uint32_t hle_slot_count = hle_page_size / hle_slot_size;
} // namespace iop_map

inline constexpr std::uint32_t iop_dma_channels = 14;
inline constexpr std::uint32_t chcr_busy = 0x01000000u;
inline constexpr std::uint32_t dicr_master_enable = 1u << 23;

// The trap word an HLE slot holds (an encoding MIPS I leaves undefined), for dumps and disassembly.
inline constexpr std::uint32_t iop_hle_trap_opcode = 0x1Bu << 26;

// What the hardware asks of the interpreted IOP that owns it.
class IIopHardwareOwner {
  public:
    virtual ~IIopHardwareOwner() = default;
    // The IOP's local time (EE cycles), as of the access in progress.
    virtual Cycles hw_now() const = 0;
    // An interrupt line became pending at `at` (the IOP must run its handler when it can).
    virtual void hw_interrupt(Cycles at) = 0;
    // A read of a device or controller register, for the IOP spin detector.
    virtual void hw_poll(std::uint32_t pc, std::uint32_t address, std::uint32_t value) = 0;
    // A register write: the device's state changed; it may also have scheduled events.
    virtual void hw_activity() = 0;
    // Ends the run with a message about the access at `pc`.
    [[noreturn]] virtual void hw_stop(std::string message, std::uint32_t pc, std::uint32_t address) = 0;
};

class IopHardware final : public IIopCpuBus, public IIopHardwareHost {
  public:
    IopHardware(MachineServices& machine, IopMemory& ram, IIopHardwareOwner& owner,
                std::vector<IopDeviceMapping> devices);
    IopHardware(const IopHardware&) = delete;
    IopHardware& operator=(const IopHardware&) = delete;

    // Connects the devices that need the host (Iop::attach).
    void connect_devices();
    // Power-on (and IOP reboot): controllers cleared, transfers in flight forgotten.
    void reset();
    // How many HLE slots the page has; fetches beyond them are bus errors.
    void set_hle_slots(std::uint32_t count) noexcept { hle_slots_ = count; }

    // IIopCpuBus.
    bool read(std::uint32_t physical, std::uint32_t width, std::uint32_t& value, std::uint32_t pc) override;
    bool write(std::uint32_t physical, std::uint32_t width, std::uint32_t value, std::uint32_t pc) override;
    std::optional<std::uint32_t> hle_slot(std::uint32_t physical) const override;

    // IIopHardwareHost.
    IopMemory& ram() override { return ram_; }
    MachineServices& machine() override { return machine_; }
    Cycles iop_now() const override;
    void raise_irq(IopIrq line, Cycles at) override;
    void dma_complete(std::uint32_t channel, Cycles at) override;

    // The interrupt controller as the HLE intrman sees it.
    std::uint32_t pending_lines() const noexcept { return (i_ctrl_ & 1u) != 0 ? (i_stat_ & i_mask_) : 0; }
    void acknowledge_line(std::uint32_t line) noexcept { i_stat_ &= ~(1u << line); }
    std::uint32_t i_mask() const noexcept { return i_mask_; }
    void set_i_mask(std::uint32_t mask) noexcept { i_mask_ = mask; }
    // DMA interrupt flags (bit n: channel n finished with its interrupt enabled), and their acknowledgement.
    std::uint32_t dma_flags() const noexcept;
    void acknowledge_dma(std::uint32_t channel) noexcept;
    bool dma_interrupt_enabled(std::uint32_t channel) const noexcept;
    void set_dma_interrupt(std::uint32_t channel, bool enabled) noexcept;

    // For messages: "SPU2+0x01A0", "DMA4 CHCR", "unmapped".
    std::string describe(std::uint32_t physical) const;
    std::uint64_t device_accesses() const noexcept { return device_accesses_; }

  private:
    struct Channel {
        std::uint32_t madr = 0;
        std::uint32_t bcr = 0;
        std::uint32_t chcr = 0;
        std::uint32_t tadr = 0;
        EventId event = 0; // the completion of a transfer that is not deferred
    };

    const IopDeviceMapping* device_at(std::uint32_t physical) const;
    std::optional<std::uint32_t> register_read(std::uint32_t physical, std::uint32_t pc);
    bool register_write(std::uint32_t physical, std::uint32_t value, std::uint32_t pc);
    static std::optional<std::uint32_t> dma_register(std::uint32_t physical, std::uint32_t& channel);
    void write_chcr(std::uint32_t channel, std::uint32_t value, std::uint32_t pc);
    void start_dma(std::uint32_t channel, std::uint32_t pc);
    bool dpcr_enabled(std::uint32_t channel) const noexcept;
    std::uint32_t dicr_read() const noexcept;

    MachineServices& machine_;
    IopMemory& ram_;
    IIopHardwareOwner& owner_;
    std::vector<IopDeviceMapping> devices_;
    std::array<int, iop_dma_channels> dma_device_{}; // index into devices_, -1 for none
    std::array<std::uint8_t, iop_map::scratchpad_size> scratchpad_{};
    std::array<std::uint32_t, iop_map::ssbus_words> ssbus_a_{};
    std::array<std::uint32_t, iop_map::ssbus_words> ssbus_b_{};
    std::uint32_t i_stat_ = 0;
    std::uint32_t i_mask_ = 0;
    std::uint32_t i_ctrl_ = 1;
    std::array<Channel, iop_dma_channels> channels_{};
    std::uint32_t dpcr_ = 0;
    std::uint32_t dicr_ = 0;
    std::uint32_t dpcr2_ = 0;
    std::uint32_t dicr2_ = 0;
    std::uint32_t dmacen_ = 0;
    std::uint32_t dmacinten_ = 0;
    std::uint32_t hle_slots_ = 0;
    std::uint64_t device_accesses_ = 0;
};

} // namespace dq8::rt
