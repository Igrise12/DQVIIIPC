// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/machine/machine.h"

#include "runtime/ee/dispatcher.h"
#include "runtime/ee/ee_runtime.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/kernel/ee_kernel.h"
#include "runtime/kernel/ee_services.h"
#include "runtime/kernel/fiber.h"
#include "runtime/kernel/kernel_services.h"
#include "runtime/machine/events.h"
#include "runtime/machine/interrupts.h"
#include "runtime/machine/mmio.h"
#include "runtime/machine/spin_detector.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dq8::rt {

struct Machine::Impl {
    Impl(Machine& self, GuestMemory& memory_, Dispatcher& dispatcher_, MachineConfig config_)
        : memory(memory_), dispatcher(dispatcher_), config(std::move(config_)), spin(config.cost.spin_reads),
          ee(memory, dispatcher, self, kernel_services),
          kernel(self, ee,
                 EeKernel::Options{config.max_call_depth, config.no_progress_fields, FiberStackSpec{}}) {}

    void schedule_vblank_start(std::uint64_t field) {
        events.schedule(config.video.field_start(field), "vblank-start",
                        [this, field](Cycles) { vblank_start(field); });
    }

    // Vblank start of `field`: the listeners in phase order (gs, kernel, iop, intc), the kernel's own checks,
    // then the matching vblank end and the next field's start.
    void vblank_start(std::uint64_t field) {
        vblanks = field + 1;
        for (auto& phase : listeners) {
            for (IVblankListener* l : phase) {
                l->on_vblank_start(field);
            }
        }
        kernel.on_vblank_start(field);
        events.schedule(config.video.vblank_end(field), "vblank-end", [this, field](Cycles) {
            for (auto& phase : listeners) {
                for (IVblankListener* l : phase) {
                    l->on_vblank_end(field);
                }
            }
        });
        schedule_vblank_start(field + 1);
    }

    GuestMemory& memory;
    Dispatcher& dispatcher;
    MachineConfig config;
    MmioBus mmio;
    VirtualClock clock;
    EventQueue events;
    InterruptController interrupts;
    KernelServices kernel_services;
    std::array<std::vector<IVblankListener*>, 4> listeners{};
    std::vector<IDevice*> devices;
    std::function<void(const MachineEvent&)> sink;
    SpinDetector spin;
    std::uint64_t vblanks = 0;
    bool booted = false;
    // Last: they hold references to everything above, and the kernel's fibers die before the runtime.
    EeRuntime ee;
    EeKernel kernel;
};

Machine::Machine(GuestMemory& memory, Dispatcher& dispatcher, MachineConfig config)
    : impl_(std::make_unique<Impl>(*this, memory, dispatcher, std::move(config))) {
    Impl& m = *impl_;
    m.kernel_services.set_strict(m.config.strict_kernel);
    // An interrupt that becomes pending makes the next safe point yield to the scheduler.
    m.interrupts.set_raise_hook([&m] { m.clock.force_safe_point(); });
    // An event scheduled during a slice (an RPC reply while the guest polls RAM for it) lowers the deadline,
    // so the next safe point after its time yields to the scheduler and runs it. Found by session E's
    // synthetic run: without it every RPC round trip waited for the next vblank.
    m.events.set_schedule_hook([&m](Cycles at) {
        if (at < m.clock.deadline()) {
            m.clock.set_deadline(at);
        }
    });
    m.ee.attach_kernel(m.kernel);
    RunOptions options;
    options.stop_at_first_syscall = m.config.stop_at_first_syscall;
    options.max_call_depth = m.config.max_call_depth;
    options.trace_dispatch = m.config.trace_dispatch;
    m.ee.set_options(options);
    register_ee_kernel_services(m.kernel_services, m.kernel);
    // Device addresses go through the bus; a handled access charges the cost model's MMIO cost, an unhandled
    // one stays M2's logged hole (Docs/milestones/M3-boots.md, "Devices").
    memory.set_mmio_bus(&m.mmio, &m.clock, m.config.cost.mmio_access);
}

Machine::~Machine() {
    // The memory usually outlives the machine: leave it without a dangling bus.
    impl_->memory.set_mmio_bus(nullptr, nullptr, 0);
}

void Machine::attach(IDevice& device) {
    if (impl_->booted) {
        throw std::logic_error("Machine::attach after boot");
    }
    impl_->devices.push_back(&device);
    device.attach(*this);
}

void Machine::set_event_sink(std::function<void(const MachineEvent&)> sink) {
    impl_->sink = std::move(sink);
}

void Machine::boot(std::uint32_t entry) {
    Impl& m = *impl_;
    if (Fiber::current() != nullptr) {
        throw std::logic_error("Machine::boot on a guest fiber");
    }
    m.clock.reset();
    m.events.clear();
    m.interrupts.reset();
    m.spin.reset();
    m.vblanks = 0;
    for (IDevice* d : m.devices) {
        d->reset();
    }
    m.kernel.reset(entry);
    m.schedule_vblank_start(0);
    m.booted = true;
}

SliceResult Machine::run(const RunLimits& limits) {
    Impl& m = *impl_;
    if (!m.booted) {
        throw std::logic_error("Machine::run before boot");
    }
    if (Fiber::current() != nullptr) {
        throw std::logic_error("Machine::run on a guest fiber");
    }
    m.ee.set_max_dispatches(limits.max_dispatches);
    m.kernel.run([&m, &limits] { return limits.until_vblank != 0 && m.vblanks >= limits.until_vblank; });
    SliceResult r;
    r.stopped = m.kernel.stopped();
    if (r.stopped) {
        r.stop = *m.kernel.stop_info();
    }
    r.vblank = m.vblanks;
    r.cycles = m.clock.now();
    r.dispatches = m.ee.dispatches();
    return r;
}

EeRuntime& Machine::ee() {
    return impl_->ee;
}

GuestMemory& Machine::memory() {
    return impl_->memory;
}

MmioBus& Machine::mmio() {
    return impl_->mmio;
}

VirtualClock& Machine::clock() {
    return impl_->clock;
}

EventQueue& Machine::events() {
    return impl_->events;
}

InterruptController& Machine::interrupts() {
    return impl_->interrupts;
}

KernelServices& Machine::kernel() {
    return impl_->kernel_services;
}

const CostModel& Machine::cost() const {
    return impl_->config.cost;
}

const VideoTiming& Machine::video() const {
    return impl_->config.video;
}

Scheduler* Machine::scheduler() {
    return &impl_->kernel;
}

void Machine::add_vblank_listener(VblankPhase phase, IVblankListener& listener) {
    impl_->listeners[static_cast<std::size_t>(phase)].push_back(&listener);
}

std::uint64_t Machine::vblank_count() const {
    return impl_->vblanks;
}

void Machine::note_poll(std::uint32_t pc, std::uint32_t address, std::uint64_t value) {
    Impl& m = *impl_;
    if (!m.spin.note(pc, address, value)) {
        return;
    }
    // A poll that cannot observe a change: jump to the next event (at most one quantum) and make the next
    // safe point run it.
    if (const auto next = m.events.next_time(); next && *next > m.clock.now()) {
        m.clock.advance_to(std::min(*next, m.clock.now() + m.config.cost.spin_quantum));
    }
    m.clock.force_safe_point();
}

void Machine::note_activity() {
    impl_->spin.reset();
}

void Machine::emit(MachineEvent event) {
    if (impl_->sink) {
        impl_->sink(event);
    }
}

bool Machine::tracing(TraceCategory category) const {
    return (impl_->config.trace_mask & trace_bit(category)) != 0;
}

void Machine::stop(StopInfo info) {
    impl_->kernel.stop(std::move(info));
}

} // namespace dq8::rt
