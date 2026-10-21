// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/ee/guest_stop.h"
#include "runtime/machine/clock.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// What every device and service sees of the machine (Docs/milestones/M3-boots.md, "Contracts"). The Machine
// (machine.h) implements it; tests use tests/unit/support/fake_machine.h. Devices are attached by the host in
// a fixed order and register everything they need from attach(): MMIO ranges, kernel services, vblank
// listeners. The machine itself never names a peripheral class.
namespace dq8::rt {

class GuestMemory;
class MmioBus;
class EventQueue;
class InterruptController;
class KernelServices;
class Scheduler;
struct CostModel;
struct VideoTiming;

// One JSON-lines event for the host (architecture §16). The host adds "ev", "vblank" and "cycles"; fields
// must be deterministic (no wall time, no host pointers).
struct MachineEvent {
    using Value = std::variant<std::int64_t, std::uint64_t, bool, std::string>;
    std::string type; // "thread", "iop_module", "rpc_bind", "overlay_resident", ...
    std::vector<std::pair<std::string, Value>> fields;

    MachineEvent& add(std::string key, Value value) {
        fields.emplace_back(std::move(key), std::move(value));
        return *this;
    }
};

// The fixed order in which vblank-start work happens (architecture §5.5): GS privileged registers (CSR field
// and VSINT, the vsync snapshot for the backend), then the kernel's SetVSyncFlag targets, then the IOP's pad
// DMA, then the INTC vblank-start cause. Listeners of one phase run in registration order.
enum class VblankPhase : std::uint8_t { gs = 0, kernel = 1, iop = 2, intc = 3 };

class IVblankListener {
  public:
    virtual ~IVblankListener() = default;
    // `field` counts fields from boot, starting at 0.
    virtual void on_vblank_start(std::uint64_t field) = 0;
    virtual void on_vblank_end(std::uint64_t /*field*/) {}
};

class MachineServices {
  public:
    virtual ~MachineServices() = default;

    virtual GuestMemory& memory() = 0;
    virtual MmioBus& mmio() = 0;
    virtual VirtualClock& clock() = 0;
    virtual EventQueue& events() = 0;
    virtual InterruptController& interrupts() = 0;
    virtual KernelServices& kernel() = 0;
    virtual const CostModel& cost() const = 0;
    virtual const VideoTiming& video() const = 0;
    // Null where no guest threads exist (device unit tests).
    virtual Scheduler* scheduler() = 0;

    virtual void add_vblank_listener(VblankPhase phase, IVblankListener& listener) = 0;
    // Fields started since boot (vblank_start events delivered).
    virtual std::uint64_t vblank_count() const = 0;

    // A read of a volatile register: feeds the spin detector (spin_detector.h), which may jump the clock.
    virtual void note_poll(std::uint32_t pc, std::uint32_t address, std::uint64_t value) = 0;
    // Something guest-visible changed (a register write, a completion): resets the spin detector.
    virtual void note_activity() = 0;

    virtual void emit(MachineEvent event) = 0;
    // Ends the run with a diagnostic. On a guest fiber this switches away and never returns; elsewhere it
    // throws (the machine catches it). Never returns either way.
    [[noreturn]] virtual void stop(StopInfo info) = 0;
};

// A peripheral or service group the host attaches to the machine.
class IDevice {
  public:
    virtual ~IDevice() = default;
    virtual std::string_view name() const = 0;
    // Called once, in attach order: map MMIO ranges, register kernel services and vblank listeners.
    virtual void attach(MachineServices& machine) = 0;
    // Power-on state. Called at boot, after every device is attached, in attach order.
    virtual void reset() = 0;
};

} // namespace dq8::rt
