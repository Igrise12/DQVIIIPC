// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/machine/clock.h"
#include "runtime/machine/cost_model.h"
#include "runtime/machine/machine_services.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

// The machine: one deterministic machine thread that runs EE guest threads on fibers, the attached devices,
// the event queue and the virtual clock (architecture §12, ADR-0007). The host builds it, attaches devices in
// a fixed order, boots it and runs it in resumable slices; the limiter and presentation live outside, between
// slices. Implemented by M3 session A (machine.cpp).
namespace dq8::rt {

class GuestMemory;
class Dispatcher;
class EeRuntime;

struct MachineConfig {
    CostModel cost{};
    VideoTiming video = VideoTiming::ntsc();
    std::uint32_t max_call_depth = 4096; // nested dispatched calls per fiber
    bool strict_kernel = false;          // noop kernel services stop (KernelServices::set_strict)
    bool stop_at_first_syscall = false;  // M2 behaviour: stop at the first kernel call, implemented or not
    bool trace_dispatch = false;
    std::uint32_t trace_mask = 0; // trace_bit(TraceCategory) bits (machine_services.h)
};

struct RunLimits {
    std::uint64_t until_vblank = 0;   // end the slice once vblank_count() reaches this; 0 = no vblank limit
    std::uint64_t max_dispatches = 0; // end the run with StopReason::max_dispatches; 0 = no limit
};

struct SliceResult {
    bool stopped = false;         // the run is over; `stop` says why
    std::optional<StopInfo> stop; // set when stopped
    std::uint64_t vblank = 0;     // vblank_count() at the end of the slice
    Cycles cycles = 0;            // clock().now() at the end of the slice
    std::uint64_t dispatches = 0; // since boot
};

class Machine final : public MachineServices {
  public:
    Machine(GuestMemory& memory, Dispatcher& dispatcher, MachineConfig config = {});
    ~Machine() override;
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    // Before boot(), in order; the device's attach() runs now and must outlive the machine.
    void attach(IDevice& device);
    // JSON-lines events from emit(), with vblank and cycles added by the caller of the sink.
    void set_event_sink(std::function<void(const MachineEvent&)> sink);

    // Resets every attached device and the kernel, then creates the boot thread at `entry` with the power-on
    // register state (reset_context in runtime/ee/context.h).
    void boot(std::uint32_t entry);
    // Runs until a limit is reached or the guest stops. Resumable: call again to continue after a limit.
    SliceResult run(const RunLimits& limits);

    // The dq8_runtime_api host generated code talks to.
    EeRuntime& ee();

    // MachineServices.
    GuestMemory& memory() override;
    MmioBus& mmio() override;
    VirtualClock& clock() override;
    EventQueue& events() override;
    InterruptController& interrupts() override;
    KernelServices& kernel() override;
    const CostModel& cost() const override;
    const VideoTiming& video() const override;
    Scheduler* scheduler() override;
    void add_vblank_listener(VblankPhase phase, IVblankListener& listener) override;
    std::uint64_t vblank_count() const override;
    void note_poll(std::uint32_t pc, std::uint32_t address, std::uint64_t value) override;
    void note_activity() override;
    void emit(MachineEvent event) override;
    bool tracing(TraceCategory category) const override;
    [[noreturn]] void stop(StopInfo info) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dq8::rt
