// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/ee/guest_stop.h"

#include <cstdint>

// The EE thread scheduler (architecture §12.3): priority-only like the EE kernel (lower number first, FIFO
// within a priority, no time slicing), guest threads on fibers, one handler fiber for INTC, DMAC and alarm
// handlers. Interrupts and switches happen only at safe points: eeCheckpointDue, dispatched branches, kernel
// calls, and the run loop. Implemented by M3 session A; this is the surface other code may rely on.
namespace dq8::rt {

class Scheduler {
  public:
    virtual ~Scheduler() = default;

    // The running guest thread's id, or 0 on the scheduler stack.
    virtual std::int32_t current_thread_id() const noexcept = 0;
    // True while an interrupt or alarm handler runs (the handler fiber).
    virtual bool in_handler() const noexcept = 0;
    // Asks for a scheduling decision at the end of the current kernel call or at the next safe point (a
    // higher-priority thread became ready, an interrupt was raised).
    virtual void request_reschedule() noexcept = 0;
    // Stops the machine (MachineServices::stop forwards here once threads exist).
    [[noreturn]] virtual void stop(StopInfo info) = 0;
};

} // namespace dq8::rt
