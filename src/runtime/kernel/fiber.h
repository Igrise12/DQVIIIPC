// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// Guest threads run on fibers (architecture §12.3, ADR-0007): each has its own native stack, so a blocking
// kernel call is a switch back to the scheduler and recompiled code never unwinds. Fibers are asymmetric:
// only the scheduler (on the machine thread's own stack) resumes them, and a fiber only yields back to it.
// Backend chosen by DQ8_FIBER_BACKEND: minicoro (default), ucontext or win32. Implemented by M3 session A.
//
// Two invariants every caller keeps:
//   1. Never yield while a C++ exception is in flight or from inside a catch block (the caught-exception
//   state
//      is per thread, not per fiber): copy what you need out of the handler, leave it, then yield.
//   2. Host frames on a guest fiber must be abandonable: a stopped or exited thread's fiber is destroyed
//   while
//      suspended, without unwinding, so no locks, owned heap memory or open handles may live on those frames.
namespace dq8::rt {

struct FiberStackSpec {
    std::size_t reserve = std::size_t{8} << 20; // bytes of address space; 32 MiB in sanitizer builds
    bool guard_page = true;                     // an inaccessible page below the stack catches overflow
};

class Fiber {
  public:
    using Entry = void (*)(void* arg);

    // Throws std::runtime_error when the stack cannot be allocated.
    static std::unique_ptr<Fiber> create(Entry entry, void* arg, const FiberStackSpec& spec = {});
    virtual ~Fiber() = default;

    // From the scheduler only: runs the fiber until it yields or its entry returns.
    virtual void resume() = 0;
    // From inside a fiber: back to whoever resumed it.
    static void yield();
    // The running fiber, or null on the scheduler's own stack.
    static Fiber* current() noexcept;

    virtual bool finished() const noexcept = 0;
    // The lowest usable stack address (above the guard page), for the headroom check on dispatched calls.
    virtual std::uintptr_t stack_low() const noexcept = 0;
};

} // namespace dq8::rt
