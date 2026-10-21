// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// rt::Fiber on Win32 fibers (DQ8_FIBER_BACKEND=win32), a fallback for Windows. The system manages the stack
// and its guard page; the machine thread becomes a fiber the first time it resumes one. Not built in CI: the
// default backend on Windows is minicoro.
#include "runtime/kernel/fiber.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace dq8::rt {

namespace {

class Win32Fiber;
thread_local Win32Fiber* t_current = nullptr;
thread_local void* t_scheduler_fiber = nullptr;

class Win32Fiber final : public Fiber {
  public:
    Win32Fiber(Entry entry, void* arg, const FiberStackSpec& spec)
        : entry_(entry), arg_(arg), reserve_(spec.reserve) {
        fiber_ =
            CreateFiberEx(64 * 1024, spec.reserve, FIBER_FLAG_FLOAT_SWITCH, &Win32Fiber::trampoline, this);
        if (fiber_ == nullptr) {
            throw std::runtime_error("fiber: CreateFiberEx failed");
        }
    }

    ~Win32Fiber() override {
        if (running_) {
            std::fputs("dq8: a running fiber was destroyed\n", stderr);
            std::abort();
        }
        DeleteFiber(fiber_);
    }

    void resume() override {
        if (finished_) {
            throw std::logic_error("fiber: resume of a finished fiber");
        }
        if (t_scheduler_fiber == nullptr) {
            t_scheduler_fiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);
            if (t_scheduler_fiber == nullptr) {
                throw std::runtime_error("fiber: ConvertThreadToFiber failed");
            }
        }
        Win32Fiber* const previous = t_current;
        caller_ = GetCurrentFiber();
        t_current = this;
        running_ = true;
        SwitchToFiber(fiber_);
        running_ = false;
        t_current = previous;
    }

    void yield_from_inside() { SwitchToFiber(caller_); }

    bool finished() const noexcept override { return finished_; }
    std::uintptr_t stack_low() const noexcept override { return low_; }
    std::uintptr_t stack_high() const noexcept override { return high_; }

  private:
    static void WINAPI trampoline(void* param) {
        auto* self = static_cast<Win32Fiber*>(param);
        const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
        self->high_ = reinterpret_cast<std::uintptr_t>(tib->StackBase);
        // The reservation ends `reserve` bytes below the base; keep three pages for the system guard region.
        self->low_ = self->high_ - self->reserve_ + 3 * 4096;
        self->entry_(self->arg_);
        self->finished_ = true;
        SwitchToFiber(self->caller_); // a fiber procedure must never return
        std::abort();
    }

    Entry entry_;
    void* arg_;
    std::size_t reserve_;
    void* fiber_ = nullptr;
    void* caller_ = nullptr;
    bool finished_ = false;
    bool running_ = false;
    std::uintptr_t low_ = 0;
    std::uintptr_t high_ = 0;
};

} // namespace

std::unique_ptr<Fiber> Fiber::create(Entry entry, void* arg, const FiberStackSpec& spec) {
    if (entry == nullptr) {
        throw std::invalid_argument("fiber: null entry");
    }
    return std::make_unique<Win32Fiber>(entry, arg, spec);
}

void Fiber::yield() {
    Win32Fiber* const self = t_current;
    if (self == nullptr) {
        throw std::logic_error("fiber: yield outside any fiber");
    }
    self->yield_from_inside();
}

Fiber* Fiber::current() noexcept {
    return t_current;
}

} // namespace dq8::rt
