// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// Guest threads run on fibers (architecture §12.3, ADR-0007): each has its own native stack, so a blocking
// kernel call is a switch back to the scheduler and recompiled code never unwinds. Fibers are asymmetric:
// only the scheduler (on the machine thread's own stack) resumes them, and a fiber only yields back to it.
// Backend chosen by DQ8_FIBER_BACKEND: minicoro (default, fiber_minicoro.cpp), ucontext (fiber_ucontext.cpp)
// or win32 (fiber_win32.cpp); exactly one of them is compiled.
//
// Two invariants every caller keeps:
//   1. Never yield while a C++ exception is in flight or from inside a catch block (the caught-exception
//      state is per thread, not per fiber): copy what you need out of the handler, leave it, then yield.
//   2. Host frames on a guest fiber must be abandonable: a stopped or exited thread's fiber is destroyed
//      while suspended, without unwinding, so no locks, owned heap memory or open handles may live on those
//      frames.
namespace dq8::rt {

// DQ8_FIBER_ASAN: set by the build for the one backend file it compiles without instrumentation in an
// AddressSanitizer build (that file annotates the switches itself).
#if defined(__SANITIZE_ADDRESS__) || defined(DQ8_FIBER_ASAN)
#define DQ8_FIBER_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DQ8_FIBER_SANITIZED 1
#endif
#endif

// Sanitizers make frames several times larger, so their builds reserve more address space per fiber.
#if defined(DQ8_FIBER_SANITIZED)
inline constexpr std::size_t default_fiber_reserve = std::size_t{32} << 20;
#else
inline constexpr std::size_t default_fiber_reserve = std::size_t{8} << 20;
#endif

struct FiberStackSpec {
    std::size_t reserve = default_fiber_reserve; // bytes of address space, guard page included
    bool guard_page = true;                      // an inaccessible page below the stack catches overflow
};

class Fiber {
  public:
    using Entry = void (*)(void* arg);

    // Throws std::runtime_error when the stack cannot be allocated, std::invalid_argument for a null entry or
    // a reserve too small to hold a stack.
    static std::unique_ptr<Fiber> create(Entry entry, void* arg, const FiberStackSpec& spec = {});
    // Destroys the fiber whether it finished or is suspended (its frames are abandoned, invariant 2). Never
    // destroy the running fiber.
    virtual ~Fiber() = default;
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;

    // From the scheduler only: runs the fiber until it yields or its entry returns. Resuming a finished
    // fiber throws std::logic_error.
    virtual void resume() = 0;
    // From inside a fiber: back to whoever resumed it. Throws std::logic_error off any fiber.
    static void yield();
    // The running fiber, or null on the scheduler's own stack.
    static Fiber* current() noexcept;

    virtual bool finished() const noexcept = 0;
    // The lowest usable stack address (above the guard page), for the headroom check on dispatched calls.
    virtual std::uintptr_t stack_low() const noexcept = 0;
    // One past the highest usable stack address.
    virtual std::uintptr_t stack_high() const noexcept = 0;

    // Bytes left between the caller's frame and stack_low() when called on this fiber's stack.
    std::size_t headroom() const noexcept;

  protected:
    Fiber() = default;
};

// The address of the caller's own frame, for headroom checks (not a local variable's address:
// AddressSanitizer may keep locals on a separate fake stack).
std::uintptr_t current_frame_address() noexcept;

} // namespace dq8::rt
