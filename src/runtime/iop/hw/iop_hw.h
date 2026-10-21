// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/machine/clock.h"

#include <cstdint>
#include <string_view>
#include <vector>

// The seam between the interpreted IOP (M4 session A) and the IOP-side devices (the SPU2, session B), fixed
// on M4 day 0 (Docs/milestones/M4-playable.md, "Contracts"). The IOP owns its bus, interrupt controller and
// DMA controller; a device plugs into them through these interfaces and never names the IOP's classes, and
// the IOP never names a device class: the host hands IopConfig::devices a list of mappings.
//
// Time: every access and request carries the IOP's local time in EE cycles (machine/clock.h; one IOP cycle is
// ee_cycles_per_iop_cycle EE cycles). Accesses arrive in non-decreasing time order. A device that does work
// later schedules events on the machine's EventQueue and reports with explicit times; nothing completes
// instantly.
namespace dq8::rt {

class IopMemory;
class MachineServices;

// Where and when an IOP access happens.
struct IopAccess {
    Cycles at = 0;        // IOP-local time (EE cycles)
    std::uint32_t pc = 0; // IOP pc of the access
};

// A device on the IOP bus (the SPU2 at 0x1F900000). Offsets are from the mapping's base; widths are 1, 2 or 4
// bytes (32-bit accesses to a 16-bit device arrive as two 16-bit accesses). Returning false marks the access
// unhandled: the IOP stops with the device name, offset and IOP pc.
class IIopBusDevice {
  public:
    virtual ~IIopBusDevice() = default;
    virtual std::string_view name() const = 0;
    virtual bool read(std::uint32_t offset, std::uint32_t width, std::uint32_t& value,
                      const IopAccess& at) = 0;
    virtual bool write(std::uint32_t offset, std::uint32_t width, std::uint32_t value,
                       const IopAccess& at) = 0;
};

// IOP interrupt controller lines (I_STAT/I_MASK bits).
enum class IopIrq : std::uint8_t {
    vblank = 0,
    cdvd = 2,
    dma = 3,
    timer0 = 4,
    timer1 = 5,
    timer2 = 6,
    spu2 = 9,
    evblank = 11,
    timer3 = 14,
    timer4 = 15,
    timer5 = 16,
    sio2 = 17,
};

// A DMA start on an IOP channel a device serves (channels 4 and 7 for the SPU2's two cores): the channel
// registers as the guest wrote them.
struct IopDmaRequest {
    std::uint32_t channel = 0;
    std::uint32_t madr = 0;
    std::uint32_t bcr = 0;
    std::uint32_t chcr = 0;
    Cycles at = 0;
};

// How the device takes a DMA start: done at a known time, or deferred (it calls
// IIopHardwareHost::dma_complete itself later, as the SPU2's AutoDMA does).
struct IopDmaStart {
    bool deferred = false;
    Cycles done_at = 0;
};

class IIopDmaDevice {
  public:
    virtual ~IIopDmaDevice() = default;
    // Data moves through IIopHardwareHost::ram(). Called with the channel's busy bit set.
    virtual IopDmaStart dma_start(const IopDmaRequest& request) = 0;
    // The guest cleared the busy bit of a running transfer.
    virtual void dma_stop(std::uint32_t channel, Cycles at) = 0;
};

// What the IOP offers its devices.
class IIopHardwareHost {
  public:
    virtual ~IIopHardwareHost() = default;
    virtual IopMemory& ram() = 0;
    virtual MachineServices& machine() = 0;
    // The IOP's local time (EE cycles).
    virtual Cycles iop_now() const = 0;
    // Sets the line in I_STAT at `at` and wakes the IOP.
    virtual void raise_irq(IopIrq line, Cycles at) = 0;
    // Ends a transfer: CHCR busy off, the DICR flag, INTC line dma if enabled.
    virtual void dma_complete(std::uint32_t channel, Cycles at) = 0;
};

// A device that needs the host (called from Iop::attach, before any access).
class IIopHardwareClient {
  public:
    virtual ~IIopHardwareClient() = default;
    virtual void connect(IIopHardwareHost& host) = 0;
};

// One device on the IOP side: a bus range, its DMA channels, and its hooks. `client` and `dma` may be null.
struct IopDeviceMapping {
    std::uint32_t base = 0;
    std::uint32_t size = 0;
    IIopBusDevice* device = nullptr;
    IIopHardwareClient* client = nullptr;
    IIopDmaDevice* dma = nullptr;
    std::vector<std::uint32_t> dma_channels;
};

} // namespace dq8::rt
