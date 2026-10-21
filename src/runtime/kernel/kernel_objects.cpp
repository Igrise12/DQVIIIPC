// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors: the thread, semaphore,
// event-flag, alarm and handler-chain operations of ps2xRuntime/src/lib/Kernel/EeScheduler.cpp. What changed:
// the five transfers by exception became block_current() and switch_out(); alarms count hblanks of the
// virtual clock; StartThread never allocates a stack (the service stops for a thread without one).
#include "runtime/ee/context.h"
#include "runtime/ee/ee_runtime.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/kernel/ee_kernel.h"
#include "runtime/kernel/kernel_abi.h"
#include "runtime/machine/machine_services.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace dq8::rt {

namespace {

template <typename Map> std::int32_t allocate_id(std::int32_t& next, const Map& objects) {
    const std::int32_t first = std::max(1, next);
    std::int32_t candidate = first;
    do {
        if (objects.find(candidate) == objects.end()) {
            next = candidate == std::numeric_limits<std::int32_t>::max() ? 1 : candidate + 1;
            return candidate;
        }
        candidate = candidate == std::numeric_limits<std::int32_t>::max() ? 1 : candidate + 1;
    } while (candidate != first);
    return 0;
}

bool event_condition(std::uint32_t current, std::uint32_t requested, std::uint32_t mode) noexcept {
    return (mode & kabi::event_mode_or) != 0 ? (current & requested) != 0
                                             : (current & requested) == requested;
}

void apply_clear_mode(std::uint32_t& bits, std::uint32_t requested, std::uint32_t mode) noexcept {
    if ((mode & kabi::event_mode_clear_all) != 0) {
        bits = 0;
    } else if ((mode & kabi::event_mode_clear) != 0) {
        bits &= ~requested;
    }
}

void set_result(R5900Context& ctx, std::int32_t value) noexcept {
    set_gpr_lo(ctx, 2, static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
}

} // namespace

// ---- Ready queues and waits.

GuestThread* EeKernel::thread(std::int32_t id) {
    const auto it = threads_.find(id);
    return it == threads_.end() ? nullptr : &it->second;
}

const GuestThread* EeKernel::thread(std::int32_t id) const {
    const auto it = threads_.find(id);
    return it == threads_.end() ? nullptr : &it->second;
}

void EeKernel::enqueue_ready(GuestThread& t, bool front) {
    t.status = ThreadStatus::ready;
    auto& queue = ready_[static_cast<std::size_t>(t.current_priority)];
    if (front) {
        queue.push_front(t.id);
    } else {
        queue.push_back(t.id);
    }
}

void EeKernel::remove_ready(GuestThread& t) {
    if (t.status != ThreadStatus::ready) {
        return;
    }
    auto& queue = ready_[static_cast<std::size_t>(t.current_priority)];
    const auto it = std::find(queue.begin(), queue.end(), t.id);
    if (it != queue.end()) {
        queue.erase(it);
    }
}

GuestThread* EeKernel::peek_ready() {
    for (auto& queue : ready_) {
        if (!queue.empty()) {
            return thread(queue.front());
        }
    }
    return nullptr;
}

void EeKernel::request_preemption_if_higher(const GuestThread& ready) {
    if (current_ != nullptr && ready.current_priority < current_->current_priority) {
        request_reschedule();
    }
}

void EeKernel::make_ready(GuestThread& t, std::int32_t result) {
    t.wait = WaitState{};
    set_result(*t.exec.ctx, result);
    note_progress();
    if (t.suspend_count != 0) {
        t.status = ThreadStatus::suspended;
        return;
    }
    enqueue_ready(t);
    request_preemption_if_higher(t);
}

void EeKernel::remove_from_wait_object(GuestThread& t) {
    if (t.wait.kind == WaitKind::semaphore) {
        if (const auto it = semaphores_.find(t.wait.object); it != semaphores_.end()) {
            auto& w = it->second.waiters;
            w.erase(std::remove(w.begin(), w.end(), t.id), w.end());
        }
    } else if (t.wait.kind == WaitKind::event_flag) {
        if (const auto it = event_flags_.find(t.wait.object); it != event_flags_.end()) {
            auto& w = it->second.waiters;
            w.erase(std::remove(w.begin(), w.end(), t.id), w.end());
        }
    }
    t.wait = WaitState{};
}

void EeKernel::make_dormant(GuestThread& t) {
    remove_ready(t);
    remove_from_wait_object(t);
    t.status = ThreadStatus::dormant;
    t.suspend_count = 0;
    t.wakeup_count = 0;
}

GuestThread& EeKernel::running_thread(std::string_view call) {
    if (in_handler_ || current_ == nullptr || current_->fiber == nullptr ||
        Fiber::current() != current_->fiber.get()) {
        StopInfo s;
        s.reason = StopReason::unsupported;
        s.message = std::string(call) +
                    (in_handler_ ? " needs a running thread but was called from an interrupt handler"
                                 : " needs a running thread but was called outside one");
        stop(std::move(s));
    }
    return *current_;
}

void EeKernel::block_current(WaitState wait, std::string_view call) {
    GuestThread& t = running_thread(call);
    t.wait = wait;
    t.status = t.suspend_count == 0 ? ThreadStatus::waiting : ThreadStatus::waiting_suspended;
    note_progress();
    switch_out(SwitchReason::blocked);
    // Resumed: make_ready() has put the result in $v0.
}

void EeKernel::write_guest_u32(std::uint32_t address, std::uint32_t value) {
    if (address != 0) {
        machine_.memory().store32(address, value);
    }
}

void EeKernel::erase_thread(std::int32_t id) {
    GuestThread* t = thread(id);
    if (t == nullptr) {
        return;
    }
    remove_ready(*t);
    if (last_exec_ == &t->exec) {
        last_exec_ = nullptr;
    }
    if (current_ == t) {
        current_ = nullptr;
    }
    threads_.erase(id);
}

std::uint64_t EeKernel::lines_started(Cycles t) const noexcept {
    const VideoTiming& v = machine_.video();
    const std::uint64_t per_line =
        ee_clock_hz * v.field_rate_den * 2 / (v.field_rate_num * std::uint64_t{v.lines_per_frame});
    std::uint64_t n = per_line == 0 ? 0 : t / per_line;
    while (n > 0 && v.line_start(n) > t) {
        --n;
    }
    while (v.line_start(n) <= t) {
        ++n;
    }
    return n;
}

// ---- Threads.

std::int32_t EeKernel::create_thread(const ThreadParams& p) {
    // 0 (highest) to 127, as the EE kernel takes them: Sony's libkernel creates its KernelTopThread at
    // priority 0 at start-up and drops it when CreateThread refuses (M3 bring-up). PS2Recomp refuses 0.
    if (p.priority < 0 || p.priority >= kabi::priority_count) {
        return kabi::illegal_priority;
    }
    std::int32_t id = 0;
    for (std::int32_t attempt = 0; attempt <= kabi::last_thread_id - kabi::first_thread_id; ++attempt) {
        const std::int32_t candidate = next_thread_id_;
        next_thread_id_ = candidate == kabi::last_thread_id ? kabi::first_thread_id : candidate + 1;
        if (threads_.find(candidate) == threads_.end()) {
            id = candidate;
            break;
        }
    }
    if (id == 0) {
        return kabi::error;
    }
    GuestThread t;
    t.id = id;
    t.entry = p.entry;
    t.stack = p.stack;
    t.stack_size = p.stack_size;
    t.gp = p.gp;
    t.attr = p.attr;
    t.option = p.option;
    t.initial_priority = p.priority;
    t.current_priority = p.priority;
    t.status = ThreadStatus::dormant;
    t.context = std::make_unique<R5900Context>();
    t.kernel = this;
    t.exec.id = id;
    auto [it, inserted] = threads_.emplace(id, std::move(t));
    it->second.exec.ctx = it->second.context.get();
    return id;
}

std::int32_t EeKernel::delete_thread(std::int32_t id) {
    if (id <= kabi::boot_thread_id) {
        return kabi::illegal_thread_id;
    }
    GuestThread* t = thread(id);
    if (t == nullptr) {
        return kabi::unknown_thread_id;
    }
    if (t->status != ThreadStatus::dormant) {
        return kabi::not_dormant;
    }
    machine_.emit(MachineEvent{"thread", {}}.add("op", std::string("delete")).add("id", std::int64_t{id}));
    erase_thread(id);
    return kabi::ok;
}

std::int32_t EeKernel::start_thread(std::int32_t id, std::uint32_t arg, const R5900Context& caller) {
    GuestThread* t = thread(id);
    if (t == nullptr) {
        return kabi::unknown_thread_id;
    }
    if (t->status != ThreadStatus::dormant) {
        return kabi::not_dormant;
    }
    *t->context = R5900Context();
    R5900Context& c = *t->context;
    c.pc = t->entry;
    set_gpr_lo(c, 4, static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(arg))));
    set_gpr_lo(c, 28, t->gp != 0 ? t->gp : gpr_lo(caller, 28));
    const std::uint32_t stack_top = t->stack != 0 ? (t->stack + t->stack_size) & ~0xFu : gpr_u32(caller, 29);
    set_gpr_lo(c, 29,
               static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(stack_top))));
    set_gpr_lo(c, 31, 0); // returning from the entry function exits the thread
    t->fiber.reset();
    t->exec = ExecState{};
    t->exec.id = id;
    t->exec.ctx = t->context.get();
    t->suspend_count = 0;
    t->wakeup_count = 0;
    t->wait = WaitState{};
    t->delete_on_exit = false;
    enqueue_ready(*t);
    machine_.emit(MachineEvent{"thread", {}}
                      .add("op", std::string("start"))
                      .add("id", std::int64_t{id})
                      .add("entry", std::uint64_t{t->entry})
                      .add("priority", std::int64_t{t->current_priority}));
    note_progress();
    request_preemption_if_higher(*t);
    return kabi::ok;
}

void EeKernel::exit_current(bool delete_thread) {
    GuestThread& t = running_thread(delete_thread ? "ExitDeleteThread" : "ExitThread");
    make_dormant(t);
    t.delete_on_exit = delete_thread && t.id != kabi::boot_thread_id;
    machine_.emit(MachineEvent{"thread", {}}.add("op", std::string("exit")).add("id", std::int64_t{t.id}));
    note_progress();
    switch_out(SwitchReason::exited); // the scheduler destroys this fiber; never resumed
    std::abort();
}

std::int32_t EeKernel::terminate_thread(std::int32_t id) {
    if (id == 0 || id == current_thread_id()) {
        return kabi::illegal_thread_id;
    }
    GuestThread* t = thread(id);
    if (t == nullptr) {
        return kabi::unknown_thread_id;
    }
    if (t->status == ThreadStatus::dormant) {
        return kabi::dormant;
    }
    make_dormant(*t);
    t->fiber.reset(); // suspended: its frames are abandoned (fiber.h, invariant 2)
    t->exec.fiber = nullptr;
    machine_.emit(MachineEvent{"thread", {}}.add("op", std::string("terminate")).add("id", std::int64_t{id}));
    note_progress();
    return kabi::ok;
}

std::int32_t EeKernel::suspend_thread(std::int32_t id) {
    if (id == 0) {
        id = current_thread_id();
    }
    GuestThread* t = thread(id);
    if (t == nullptr) {
        return kabi::unknown_thread_id;
    }
    if (t->status == ThreadStatus::dormant) {
        return kabi::dormant;
    }
    ++t->suspend_count;
    switch (t->status) {
    case ThreadStatus::running:
        t->status = ThreadStatus::suspended;
        if (!in_handler_ && t == current_ && Fiber::current() == t->fiber.get()) {
            note_progress();
            switch_out(SwitchReason::blocked); // until ResumeThread
        }
        // From a handler, the interrupted thread gives up the CPU when the handler returns.
        break;
    case ThreadStatus::ready:
        remove_ready(*t);
        t->status = ThreadStatus::suspended;
        break;
    case ThreadStatus::waiting:
        t->status = ThreadStatus::waiting_suspended;
        break;
    case ThreadStatus::waiting_suspended:
    case ThreadStatus::suspended:
    case ThreadStatus::dormant:
        break;
    }
    return kabi::ok;
}

std::int32_t EeKernel::resume_thread(std::int32_t id) {
    if (id == 0) {
        return kabi::illegal_thread_id;
    }
    GuestThread* t = thread(id);
    if (t == nullptr) {
        return kabi::unknown_thread_id;
    }
    if (t->suspend_count == 0) {
        return kabi::not_suspended;
    }
    if (--t->suspend_count != 0) {
        return kabi::ok;
    }
    if (t->status == ThreadStatus::waiting_suspended) {
        t->status = ThreadStatus::waiting;
    } else if (t->status == ThreadStatus::suspended) {
        enqueue_ready(*t);
        request_preemption_if_higher(*t);
    }
    note_progress();
    return kabi::ok;
}

std::int32_t EeKernel::sleep_current() {
    GuestThread& t = running_thread("SleepThread");
    if (t.wakeup_count != 0) {
        --t.wakeup_count;
        return kabi::ok;
    }
    block_current(WaitState{WaitKind::sleep, 0, 0, 0, 0}, "SleepThread");
    return static_cast<std::int32_t>(gpr_u32(*t.exec.ctx, 2));
}

std::int32_t EeKernel::wakeup_thread(std::int32_t id) {
    if (id == 0 || id == current_thread_id()) {
        return kabi::illegal_thread_id;
    }
    GuestThread* t = thread(id);
    if (t == nullptr) {
        return kabi::unknown_thread_id;
    }
    if (t->status == ThreadStatus::dormant) {
        return kabi::dormant;
    }
    if ((t->status == ThreadStatus::waiting || t->status == ThreadStatus::waiting_suspended) &&
        t->wait.kind == WaitKind::sleep) {
        make_ready(*t, kabi::ok);
    } else {
        ++t->wakeup_count;
    }
    return kabi::ok;
}

std::int32_t EeKernel::cancel_wakeup(std::int32_t id) {
    if (id == 0) {
        id = current_thread_id();
    }
    GuestThread* t = thread(id);
    if (t == nullptr) {
        return kabi::unknown_thread_id;
    }
    const auto old = static_cast<std::int32_t>(t->wakeup_count);
    t->wakeup_count = 0;
    return old;
}

std::int32_t EeKernel::change_priority(std::int32_t id, std::int32_t priority, std::int32_t& old_priority) {
    if (priority < 0 || priority >= kabi::priority_count) {
        return kabi::illegal_priority;
    }
    if (id == 0) {
        id = current_thread_id();
    }
    GuestThread* t = thread(id);
    if (t == nullptr) {
        return kabi::unknown_thread_id;
    }
    old_priority = t->current_priority;
    if (t->status == ThreadStatus::ready) {
        remove_ready(*t);
        t->current_priority = priority;
        enqueue_ready(*t);
        request_preemption_if_higher(*t);
    } else {
        t->current_priority = priority;
        if (t == current_) {
            for (std::int32_t p = 0; p < priority; ++p) {
                if (!ready_[static_cast<std::size_t>(p)].empty()) {
                    request_reschedule();
                    break;
                }
            }
        }
    }
    return kabi::ok;
}

std::int32_t EeKernel::rotate_ready_queue(std::int32_t priority) {
    if (priority == 0 && current_ != nullptr) {
        priority = current_->current_priority;
    }
    if (priority < 0 || priority >= kabi::priority_count) {
        return kabi::illegal_priority;
    }
    if (current_ != nullptr && current_->current_priority == priority) {
        GuestThread& self = *current_;
        enqueue_ready(self); // to the back of its queue: the next one of its priority runs first
        note_progress();
        if (!in_handler_ && self.fiber != nullptr && Fiber::current() == self.fiber.get()) {
            switch_out(SwitchReason::blocked);
        }
        return kabi::ok;
    }
    auto& queue = ready_[static_cast<std::size_t>(priority)];
    if (queue.size() > 1) {
        queue.push_back(queue.front());
        queue.pop_front();
    }
    return kabi::ok;
}

std::int32_t EeKernel::release_wait(std::int32_t id) {
    if (id == 0) {
        return kabi::illegal_thread_id;
    }
    GuestThread* t = thread(id);
    if (t == nullptr) {
        return kabi::unknown_thread_id;
    }
    if (t->status != ThreadStatus::waiting && t->status != ThreadStatus::waiting_suspended) {
        return kabi::not_waiting;
    }
    remove_from_wait_object(*t);
    make_ready(*t, kabi::release_wait);
    return kabi::ok;
}

void EeKernel::setup_current(std::uint32_t stack, std::uint32_t stack_size, std::uint32_t gp) {
    GuestThread* t = current_ != nullptr ? current_ : thread(kabi::boot_thread_id);
    if (t == nullptr) {
        return;
    }
    t->stack = stack;
    t->stack_size = stack_size;
    t->gp = gp;
    if (t->id == kabi::boot_thread_id) {
        main_stack_base_ = stack;
    }
}

// ---- Semaphores.

std::int32_t EeKernel::create_semaphore(std::int32_t init_count, std::int32_t max_count, std::uint32_t attr,
                                        std::uint32_t option) {
    if (max_count <= 0 || init_count < 0 || init_count > max_count) {
        return kabi::error;
    }
    const std::int32_t id = allocate_id(next_semaphore_id_, semaphores_);
    if (id == 0) {
        return kabi::error;
    }
    Semaphore s;
    s.id = id;
    s.count = init_count;
    s.max_count = max_count;
    s.init_count = init_count;
    s.attr = attr;
    s.option = option;
    semaphores_.emplace(id, std::move(s));
    return id;
}

std::int32_t EeKernel::delete_semaphore(std::int32_t id) {
    const auto it = semaphores_.find(id);
    if (it == semaphores_.end()) {
        return kabi::unknown_semaphore_id;
    }
    const std::deque<std::int32_t> waiters = std::move(it->second.waiters);
    semaphores_.erase(it);
    for (const std::int32_t w : waiters) {
        if (GuestThread* t = thread(w)) {
            make_ready(*t, kabi::wait_deleted);
        }
    }
    return id;
}

std::int32_t EeKernel::signal_semaphore(std::int32_t id) {
    const auto it = semaphores_.find(id);
    if (it == semaphores_.end()) {
        return kabi::unknown_semaphore_id;
    }
    Semaphore& s = it->second;
    if (!s.waiters.empty()) {
        const std::int32_t w = s.waiters.front();
        s.waiters.pop_front();
        if (GuestThread* t = thread(w)) {
            make_ready(*t, id); // the count passes straight to the FIFO head
        }
        return id;
    }
    if (s.count >= s.max_count) {
        return kabi::semaphore_overflow;
    }
    ++s.count;
    return id;
}

std::int32_t EeKernel::poll_semaphore(std::int32_t id) {
    const auto it = semaphores_.find(id);
    if (it == semaphores_.end()) {
        return kabi::unknown_semaphore_id;
    }
    if (it->second.count == 0) {
        return kabi::semaphore_zero;
    }
    --it->second.count;
    return id;
}

std::int32_t EeKernel::wait_semaphore(std::int32_t id) {
    const auto it = semaphores_.find(id);
    if (it == semaphores_.end()) {
        return kabi::unknown_semaphore_id;
    }
    if (it->second.count > 0) {
        --it->second.count;
        return id;
    }
    GuestThread& self = running_thread("WaitSema");
    it->second.waiters.push_back(self.id);
    block_current(WaitState{WaitKind::semaphore, id, 0, 0, 0}, "WaitSema");
    return static_cast<std::int32_t>(gpr_u32(*self.exec.ctx, 2));
}

const Semaphore* EeKernel::semaphore(std::int32_t id) const {
    const auto it = semaphores_.find(id);
    return it == semaphores_.end() ? nullptr : &it->second;
}

// ---- Event flags.

std::int32_t EeKernel::create_event_flag(std::uint32_t init_bits, std::uint32_t attr, std::uint32_t option) {
    const std::int32_t id = allocate_id(next_event_flag_id_, event_flags_);
    if (id == 0) {
        return kabi::error;
    }
    EventFlag f;
    f.id = id;
    f.attr = attr;
    f.option = option;
    f.init_bits = init_bits;
    f.bits = init_bits;
    event_flags_.emplace(id, std::move(f));
    return id;
}

std::int32_t EeKernel::delete_event_flag(std::int32_t id) {
    const auto it = event_flags_.find(id);
    if (it == event_flags_.end()) {
        return kabi::unknown_event_flag_id;
    }
    const std::deque<std::int32_t> waiters = std::move(it->second.waiters);
    event_flags_.erase(it);
    for (const std::int32_t w : waiters) {
        if (GuestThread* t = thread(w)) {
            make_ready(*t, kabi::wait_deleted);
        }
    }
    return kabi::ok;
}

void EeKernel::finish_event_waiters(EventFlag& flag) {
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();) {
        GuestThread* t = thread(*it);
        if (t == nullptr) {
            it = flag.waiters.erase(it);
            continue;
        }
        const WaitState w = t->wait;
        if (!event_condition(flag.bits, w.bits, w.mode)) {
            ++it;
            continue;
        }
        write_guest_u32(w.result_address, flag.bits); // the bits seen before the clear mode applies
        apply_clear_mode(flag.bits, w.bits, w.mode);
        it = flag.waiters.erase(it);
        make_ready(*t, kabi::ok);
    }
}

std::int32_t EeKernel::set_event_flag(std::int32_t id, std::uint32_t bits) {
    const auto it = event_flags_.find(id);
    if (it == event_flags_.end()) {
        return kabi::unknown_event_flag_id;
    }
    it->second.bits |= bits;
    finish_event_waiters(it->second);
    return kabi::ok;
}

std::int32_t EeKernel::clear_event_flag(std::int32_t id, std::uint32_t mask) {
    const auto it = event_flags_.find(id);
    if (it == event_flags_.end()) {
        return kabi::unknown_event_flag_id;
    }
    it->second.bits &= mask;
    return kabi::ok;
}

std::int32_t EeKernel::poll_event_flag(std::int32_t id, std::uint32_t bits, std::uint32_t mode,
                                       std::uint32_t& observed) {
    const auto it = event_flags_.find(id);
    if (it == event_flags_.end()) {
        return kabi::unknown_event_flag_id;
    }
    EventFlag& f = it->second;
    if (!event_condition(f.bits, bits, mode)) {
        return kabi::event_condition;
    }
    observed = f.bits;
    apply_clear_mode(f.bits, bits, mode);
    return kabi::ok;
}

std::int32_t EeKernel::wait_event_flag(std::int32_t id, std::uint32_t bits, std::uint32_t mode,
                                       std::uint32_t result_address) {
    const auto it = event_flags_.find(id);
    if (it == event_flags_.end()) {
        return kabi::unknown_event_flag_id;
    }
    EventFlag& f = it->second;
    if (event_condition(f.bits, bits, mode)) {
        write_guest_u32(result_address, f.bits);
        apply_clear_mode(f.bits, bits, mode);
        return kabi::ok;
    }
    GuestThread& self = running_thread("WaitEventFlag");
    f.waiters.push_back(self.id);
    block_current(WaitState{WaitKind::event_flag, id, bits, mode, result_address}, "WaitEventFlag");
    return static_cast<std::int32_t>(gpr_u32(*self.exec.ctx, 2));
}

const EventFlag* EeKernel::event_flag(std::int32_t id) const {
    const auto it = event_flags_.find(id);
    return it == event_flags_.end() ? nullptr : &it->second;
}

// ---- Alarms.

std::int32_t EeKernel::set_alarm(std::uint32_t ticks, std::uint32_t handler, std::uint32_t arg,
                                 std::uint32_t gp) {
    if (handler == 0) {
        return kabi::error;
    }
    const std::int32_t id = allocate_id(next_alarm_id_, alarms_);
    if (id == 0) {
        return kabi::error;
    }
    const std::uint64_t count = ticks == 0 ? 1 : ticks;
    // The ticks-th hblank boundary after now.
    const Cycles at = machine_.video().line_start(lines_started(machine_.clock().now()) - 1 + count);
    Alarm a;
    a.id = id;
    a.ticks = ticks;
    a.handler = handler;
    a.arg = arg;
    a.gp = gp;
    a.event = machine_.events().schedule(at, "alarm", [this, id](Cycles) {
        if (alarms_.find(id) != alarms_.end()) {
            pending_alarms_.push_back(id);
            machine_.clock().force_safe_point();
        }
    });
    alarms_.emplace(id, a);
    return id;
}

std::int32_t EeKernel::cancel_alarm(std::int32_t id) {
    const auto it = alarms_.find(id);
    if (it == alarms_.end()) {
        return kabi::error;
    }
    machine_.events().cancel(it->second.event);
    alarms_.erase(it);
    pending_alarms_.erase(std::remove(pending_alarms_.begin(), pending_alarms_.end(), id),
                          pending_alarms_.end());
    return kabi::ok;
}

// ---- Handler chains.

std::int32_t EeKernel::add_handler(bool dmac, std::uint32_t cause, std::uint32_t handler, std::int32_t next,
                                   std::uint32_t arg, std::uint32_t gp) {
    if (cause >= (dmac ? 16u : 15u) || handler == 0) {
        return kabi::error;
    }
    auto& handlers = dmac ? dmac_handlers_ : intc_handlers_;
    const std::int32_t id = allocate_id(dmac ? next_dmac_handler_id_ : next_intc_handler_id_, handlers);
    if (id == 0) {
        return kabi::error;
    }
    std::int32_t& head = dmac ? dmac_head_ : intc_head_;
    std::int32_t& tail = dmac ? dmac_tail_ : intc_tail_;
    IrqHandler h;
    h.id = id;
    h.cause = cause;
    h.handler = handler;
    h.arg = arg;
    h.gp = gp;
    h.order = next == 0 ? --head : ++tail;
    handlers.emplace(id, h);
    return id;
}

std::int32_t EeKernel::remove_handler(bool dmac, std::uint32_t cause, std::int32_t id) {
    auto& handlers = dmac ? dmac_handlers_ : intc_handlers_;
    if (const auto it = handlers.find(id); it != handlers.end() && it->second.cause == cause) {
        handlers.erase(it);
    }
    return kabi::ok;
}

std::int32_t EeKernel::set_handler_enabled(bool dmac, std::int32_t id, bool enabled) {
    auto& handlers = dmac ? dmac_handlers_ : intc_handlers_;
    if (const auto it = handlers.find(id); it != handlers.end()) {
        it->second.enabled = enabled;
    }
    return kabi::ok;
}

} // namespace dq8::rt
