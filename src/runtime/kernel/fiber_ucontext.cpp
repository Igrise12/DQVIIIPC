// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// rt::Fiber on POSIX ucontext (DQ8_FIBER_BACKEND=ucontext), a fallback for hosts minicoro does not cover.
// Stacks come from fiber_stack.h with the guard page at the bottom; switches are annotated for
// AddressSanitizer by hand.
#include "runtime/kernel/fiber.h"
#include "runtime/kernel/fiber_stack.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <ucontext.h>

#if defined(DQ8_FIBER_SANITIZED)
extern "C" {
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, std::size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old, std::size_t* size_old);
}
#endif

namespace dq8::rt {

namespace {

class UcontextFiber;
thread_local UcontextFiber* t_current = nullptr;

class UcontextFiber final : public Fiber {
  public:
    UcontextFiber(Entry entry, void* arg, const FiberStackSpec& spec) : entry_(entry), arg_(arg) {
        const std::size_t page = detail::page_size();
        if (spec.reserve < 2 * page) {
            throw std::invalid_argument("fiber: a stack reserve of " + std::to_string(spec.reserve) +
                                        " bytes is too small");
        }
        region_ = detail::GuardedRegion(spec.reserve, spec.guard_page, 0);
        low_ = reinterpret_cast<std::uintptr_t>(region_.data()) + page;
        high_ = reinterpret_cast<std::uintptr_t>(region_.data()) + region_.size();
        if (getcontext(&context_) != 0) {
            throw std::runtime_error("fiber: getcontext failed");
        }
        context_.uc_stack.ss_sp = reinterpret_cast<void*>(low_);
        context_.uc_stack.ss_size = high_ - low_;
        context_.uc_link = nullptr;
        const auto self = reinterpret_cast<std::uintptr_t>(this);
        // makecontext passes int arguments: the object pointer travels as two 32-bit halves.
        makecontext(&context_, reinterpret_cast<void (*)()>(&UcontextFiber::trampoline), 2,
                    static_cast<unsigned>(self & 0xFFFFFFFFu), static_cast<unsigned>(self >> 16 >> 16));
    }

    ~UcontextFiber() override {
        if (running_) {
            std::fputs("dq8: a running fiber was destroyed\n", stderr);
            std::abort();
        }
    }

    void resume() override {
        if (finished_) {
            throw std::logic_error("fiber: resume of a finished fiber");
        }
        UcontextFiber* const previous = t_current;
        t_current = this;
        running_ = true;
#if defined(DQ8_FIBER_SANITIZED)
        void* fake = nullptr;
        __sanitizer_start_switch_fiber(&fake, reinterpret_cast<const void*>(low_), high_ - low_);
#endif
        swapcontext(&caller_, &context_);
#if defined(DQ8_FIBER_SANITIZED)
        __sanitizer_finish_switch_fiber(fake, nullptr, nullptr);
#endif
        running_ = false;
        t_current = previous;
    }

    void yield_from_inside() {
#if defined(DQ8_FIBER_SANITIZED)
        void* fake = nullptr;
        __sanitizer_start_switch_fiber(&fake, caller_bottom_, caller_size_);
#endif
        swapcontext(&context_, &caller_);
#if defined(DQ8_FIBER_SANITIZED)
        __sanitizer_finish_switch_fiber(fake, &caller_bottom_, &caller_size_);
#endif
    }

    bool finished() const noexcept override { return finished_; }
    std::uintptr_t stack_low() const noexcept override { return low_; }
    std::uintptr_t stack_high() const noexcept override { return high_; }

  private:
    static void trampoline(unsigned lo, unsigned hi) {
        auto* self = reinterpret_cast<UcontextFiber*>(static_cast<std::uintptr_t>(lo) |
                                                      (static_cast<std::uintptr_t>(hi) << 16 << 16));
#if defined(DQ8_FIBER_SANITIZED)
        __sanitizer_finish_switch_fiber(nullptr, &self->caller_bottom_, &self->caller_size_);
#endif
        self->entry_(self->arg_);
        self->finished_ = true;
#if defined(DQ8_FIBER_SANITIZED)
        __sanitizer_start_switch_fiber(nullptr, self->caller_bottom_, self->caller_size_);
#endif
        setcontext(&self->caller_); // never returns
        std::abort();
    }

    Entry entry_;
    void* arg_;
    bool finished_ = false;
    bool running_ = false;
    detail::GuardedRegion region_;
    std::uintptr_t low_ = 0;
    std::uintptr_t high_ = 0;
    ucontext_t context_{};
    ucontext_t caller_{};
#if defined(DQ8_FIBER_SANITIZED)
    const void* caller_bottom_ = nullptr;
    std::size_t caller_size_ = 0;
#endif
};

} // namespace

std::unique_ptr<Fiber> Fiber::create(Entry entry, void* arg, const FiberStackSpec& spec) {
    if (entry == nullptr) {
        throw std::invalid_argument("fiber: null entry");
    }
    return std::make_unique<UcontextFiber>(entry, arg, spec);
}

void Fiber::yield() {
    UcontextFiber* const self = t_current;
    if (self == nullptr) {
        throw std::logic_error("fiber: yield outside any fiber");
    }
    self->yield_from_inside();
}

Fiber* Fiber::current() noexcept {
    return t_current;
}

} // namespace dq8::rt
