// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/ee/guest_memory.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/kernel/kernel_services.h"
#include "runtime/machine/clock.h"
#include "runtime/machine/cost_model.h"
#include "runtime/machine/events.h"
#include "runtime/machine/interrupts.h"
#include "runtime/machine/machine_services.h"
#include "runtime/machine/mmio.h"
#include "runtime/machine/spin_detector.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

// A MachineServices for device and service tests: real memory, MMIO bus, clock, events, interrupts and kernel
// registry, no guest threads (scheduler() is null). Tests drive time themselves: advance() runs events, and
// fire_vblank_start/end() call the listeners in phase order the way the machine does. stop() throws
// GuestStop, so a test can REQUIRE_THROWS_AS(..., dq8::rt::GuestStop).
namespace dq8::test {

class FakeMachine final : public rt::MachineServices {
  public:
    FakeMachine() {
        interrupts_.set_raise_hook([this] { ++raises; });
    }

    // Attaches and resets a device the way the machine will.
    void attach(rt::IDevice& device) {
        device.attach(*this);
        device.reset();
    }

    // Moves the clock to `t` and runs every event due by then.
    void advance(rt::Cycles t) {
        clock_.advance_to(t);
        events_.run_due(clock_.now());
    }
    // Runs events until the queue is empty or the next one is after `limit`.
    void run_events_until(rt::Cycles limit) {
        while (const auto next = events_.next_time()) {
            if (*next > limit) {
                break;
            }
            advance(*next);
        }
    }

    void fire_vblank_start() {
        for (auto& phase : listeners_) {
            for (rt::IVblankListener* l : phase) {
                l->on_vblank_start(vblanks_);
            }
        }
        ++vblanks_;
    }
    void fire_vblank_end() {
        for (auto& phase : listeners_) {
            for (rt::IVblankListener* l : phase) {
                l->on_vblank_end(vblanks_ == 0 ? 0 : vblanks_ - 1);
            }
        }
    }

    rt::GuestMemory& memory() override { return memory_; }
    rt::MmioBus& mmio() override { return mmio_; }
    rt::VirtualClock& clock() override { return clock_; }
    rt::EventQueue& events() override { return events_; }
    rt::InterruptController& interrupts() override { return interrupts_; }
    rt::KernelServices& kernel() override { return kernel_; }
    const rt::CostModel& cost() const override { return cost_; }
    const rt::VideoTiming& video() const override { return video_; }
    rt::Scheduler* scheduler() override { return nullptr; }
    rt::IVu0Runner* vu0() override { return vu0_; }
    void set_vu0(rt::IVu0Runner* runner) override { vu0_ = runner; }
    void add_vblank_listener(rt::VblankPhase phase, rt::IVblankListener& listener) override {
        listeners_[static_cast<std::size_t>(phase)].push_back(&listener);
    }
    std::uint64_t vblank_count() const override { return vblanks_; }
    void note_poll(std::uint32_t pc, std::uint32_t address, std::uint64_t value) override {
        ++polls;
        if (spin_.note(pc, address, value)) {
            ++spins;
            if (const auto next = events_.next_time()) {
                advance(*next);
            }
        }
    }
    void note_activity() override { spin_.reset(); }
    void emit(rt::MachineEvent event) override { emitted.push_back(std::move(event)); }
    bool tracing(rt::TraceCategory category) const override {
        return (trace_mask & rt::trace_bit(category)) != 0;
    }
    [[noreturn]] void stop(rt::StopInfo info) override { throw rt::GuestStop(std::move(info)); }

    rt::CostModel& mutable_cost() noexcept { return cost_; }

    std::vector<rt::MachineEvent> emitted;
    std::uint64_t raises = 0; // interrupt raises that left something pending
    std::uint64_t polls = 0;
    std::uint64_t spins = 0;
    std::uint32_t trace_mask = 0; // trace_bit() bits; tracing is off by default

  private:
    rt::GuestMemory memory_;
    rt::MmioBus mmio_;
    rt::VirtualClock clock_;
    rt::EventQueue events_;
    rt::InterruptController interrupts_;
    rt::KernelServices kernel_;
    rt::CostModel cost_{};
    rt::VideoTiming video_ = rt::VideoTiming::ntsc();
    rt::SpinDetector spin_{64};
    rt::IVu0Runner* vu0_ = nullptr;
    std::array<std::vector<rt::IVblankListener*>, 4> listeners_{};
    std::uint64_t vblanks_ = 0;
};

} // namespace dq8::test
