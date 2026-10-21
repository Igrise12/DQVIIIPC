// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// rt::Fiber on minicoro (ADR-0007, the default backend). minicoro's implementation is compiled here and
// nowhere else. Its coroutine block is [mco_coro | context | storage | stack]; we size the storage so that
// the stack starts two pages into a page-aligned mapping and make the page just below it, the storage's last
// page (which we never use), inaccessible: that is the guard page. minicoro saves the Windows
// thread-information block itself. Its own AddressSanitizer annotation leaves the switch back to the
// scheduler's stack (which is not a coroutine) unannotated, so in an ASan build this file is compiled without
// instrumentation (minicoro then annotates nothing) and annotates every switch here instead (DQ8_FIBER_ASAN,
// src/runtime/CMakeLists.txt).
#include "runtime/kernel/fiber.h"
#include "runtime/kernel/fiber_stack.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#define MCO_NO_DEFAULT_ALLOCATOR
#define MCO_LOG(s)
#define MINICORO_IMPL
#include <minicoro.h>

#if defined(DQ8_FIBER_ASAN)
extern "C" {
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, std::size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old, std::size_t* size_old);
}
#endif

namespace dq8::rt {

namespace {

class MinicoroFiber;
thread_local MinicoroFiber* t_current = nullptr;

class MinicoroFiber final : public Fiber {
  public:
    MinicoroFiber(Entry entry, void* arg, const FiberStackSpec& spec) : entry_(entry), arg_(arg) {
        const std::size_t page = detail::page_size();
        // What precedes the storage in the block, exactly as _mco_create_context lays it out.
        const std::size_t header =
            _mco_align_forward(sizeof(mco_coro), 16) + _mco_align_forward(sizeof(_mco_context), 16);
        if (header + 16 > page) {
            throw std::runtime_error("fiber: minicoro's coroutine header does not fit in one page");
        }
        if (spec.reserve < 2 * page + MCO_MIN_STACK_SIZE + 16) {
            throw std::invalid_argument("fiber: a stack reserve of " + std::to_string(spec.reserve) +
                                        " bytes is too small");
        }
        guard_ = spec.guard_page;
        const std::size_t stack_size = (spec.reserve - 2 * page - 16) & ~std::size_t{15};
        mco_desc desc = mco_desc_init(&MinicoroFiber::trampoline, stack_size);
        desc.storage_size = 2 * page - header; // the stack then starts at block + 2 pages
        _mco_init_desc_sizes(&desc, stack_size);
        desc.alloc_cb = &MinicoroFiber::allocate;
        desc.dealloc_cb = &MinicoroFiber::deallocate;
        desc.allocator_data = this;
        desc.user_data = this;
        const mco_result r = mco_create(&co_, &desc);
        if (r != MCO_SUCCESS) {
            co_ = nullptr;
            throw std::runtime_error(std::string("fiber: mco_create failed: ") + mco_result_description(r));
        }
        if (static_cast<std::uint8_t*>(co_->stack_base) != region_.data() + 2 * page) {
            mco_destroy(co_);
            co_ = nullptr;
            throw std::runtime_error(
                "fiber: unexpected minicoro block layout; the guard page would be misplaced");
        }
    }

    ~MinicoroFiber() override {
        if (co_ != nullptr) {
            if (mco_status(co_) == MCO_RUNNING || mco_status(co_) == MCO_NORMAL) {
                std::fputs("dq8: a running fiber was destroyed\n", stderr);
                std::abort();
            }
            mco_destroy(co_); // a suspended coroutine is abandoned without unwinding (fiber.h, invariant 2)
        }
    }

    void resume() override {
        if (finished_) {
            throw std::logic_error("fiber: resume of a finished fiber");
        }
        MinicoroFiber* const previous = t_current;
        t_current = this;
#if defined(DQ8_FIBER_ASAN)
        void* fake = nullptr;
        __sanitizer_start_switch_fiber(&fake, co_->stack_base, co_->stack_size);
#endif
        const mco_result r = mco_resume(co_);
#if defined(DQ8_FIBER_ASAN)
        __sanitizer_finish_switch_fiber(fake, nullptr, nullptr);
#endif
        t_current = previous;
        if (r != MCO_SUCCESS) {
            throw std::logic_error(std::string("fiber: mco_resume failed: ") + mco_result_description(r));
        }
        if (mco_status(co_) == MCO_DEAD) {
            finished_ = true;
        }
    }

    void yield_from_inside() {
#if defined(DQ8_FIBER_ASAN)
        void* fake = nullptr;
        __sanitizer_start_switch_fiber(&fake, caller_bottom_, caller_size_);
#endif
        const mco_result r = mco_yield(co_);
#if defined(DQ8_FIBER_ASAN)
        __sanitizer_finish_switch_fiber(fake, &caller_bottom_, &caller_size_);
#endif
        if (r != MCO_SUCCESS) {
            // The only failure here is minicoro's late stack-overflow check: the stack is already corrupt.
            std::fprintf(stderr, "dq8: fiber yield failed: %s\n", mco_result_description(r));
            std::abort();
        }
    }

    bool finished() const noexcept override { return finished_; }
    std::uintptr_t stack_low() const noexcept override {
        return reinterpret_cast<std::uintptr_t>(co_->stack_base);
    }
    std::uintptr_t stack_high() const noexcept override { return stack_low() + co_->stack_size; }

  private:
    static void trampoline(mco_coro* co) {
        auto* self = static_cast<MinicoroFiber*>(mco_get_user_data(co));
#if defined(DQ8_FIBER_ASAN)
        __sanitizer_finish_switch_fiber(nullptr, &self->caller_bottom_, &self->caller_size_);
#endif
        self->entry_(self->arg_);
#if defined(DQ8_FIBER_ASAN)
        __sanitizer_start_switch_fiber(nullptr, self->caller_bottom_, self->caller_size_); // this stack dies
#endif
    }
    static void* allocate(std::size_t size, void* data) {
        auto* self = static_cast<MinicoroFiber*>(data);
        try {
            self->region_ = detail::GuardedRegion(size, self->guard_, detail::page_size());
        } catch (const std::exception&) {
            return nullptr; // mco_create reports MCO_OUT_OF_MEMORY
        }
        return self->region_.data();
    }
    static void deallocate(void* /*ptr*/, std::size_t /*size*/, void* data) {
        static_cast<MinicoroFiber*>(data)->region_.release();
    }

    Entry entry_;
    void* arg_;
    bool guard_ = true;
    bool finished_ = false;
    detail::GuardedRegion region_;
    mco_coro* co_ = nullptr;
#if defined(DQ8_FIBER_ASAN)
    const void* caller_bottom_ = nullptr; // the scheduler stack, as AddressSanitizer reported it
    std::size_t caller_size_ = 0;
#endif
};

} // namespace

std::unique_ptr<Fiber> Fiber::create(Entry entry, void* arg, const FiberStackSpec& spec) {
    if (entry == nullptr) {
        throw std::invalid_argument("fiber: null entry");
    }
    return std::make_unique<MinicoroFiber>(entry, arg, spec);
}

void Fiber::yield() {
    MinicoroFiber* const self = t_current;
    if (self == nullptr) {
        throw std::logic_error("fiber: yield outside any fiber");
    }
    self->yield_from_inside();
}

Fiber* Fiber::current() noexcept {
    return t_current;
}

} // namespace dq8::rt
