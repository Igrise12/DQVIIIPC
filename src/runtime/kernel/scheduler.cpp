// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors: the scheduling core of
// ps2xRuntime/src/lib/Kernel/EeScheduler.cpp (selection by absolute priority then FIFO, preemption when a
// strictly higher-priority thread becomes ready, deferred selection while an interrupt handler runs). The run
// loop, the fibers, the safe points, interrupt delivery from the InterruptController, the stop path, the
// deadlock report and the watchdog are ours (ADR-0007, Docs/milestones/M3-boots.md).
#include "runtime/ee/context.h"
#include "runtime/ee/ee_runtime.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/kernel/ee_kernel.h"
#include "runtime/kernel/ee_services.h"
#include "runtime/kernel/kernel_abi.h"
#include "runtime/kernel/kernel_layout.h"
#include "runtime/machine/cost_model.h"
#include "runtime/machine/interrupts.h"
#include "runtime/machine/machine_services.h"
#include "runtime/machine/mmio.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dq8::rt {

namespace {

constexpr std::uint32_t status_ie = 1u << 0;
constexpr std::uint32_t status_exl = 1u << 1;
constexpr std::uint32_t status_erl = 1u << 2;
constexpr std::uint32_t status_eie = 1u << 16;

// An interrupt is taken only when IE and EIE are set and EXL and ERL are clear (M3-boots.md, design rules).
bool interrupts_enabled(const R5900Context& ctx) noexcept {
    const std::uint32_t s = ctx.cop0_status;
    return (s & status_ie) != 0 && (s & status_eie) != 0 && (s & (status_exl | status_erl)) == 0;
}

// DMAC handler causes beyond the ten channels: D_STAT's stall, MFIFO-empty and bus-error bits.
constexpr std::uint32_t dmac_cause_stall = 13;
constexpr std::uint32_t dmac_cause_mfifo_empty = 14;
constexpr std::uint32_t dmac_cause_bus_error = 15;

} // namespace

std::string_view to_string(ThreadStatus status) noexcept {
    switch (status) {
    case ThreadStatus::running:
        return "running";
    case ThreadStatus::ready:
        return "ready";
    case ThreadStatus::waiting:
        return "waiting";
    case ThreadStatus::waiting_suspended:
        return "waiting-suspended";
    case ThreadStatus::suspended:
        return "suspended";
    case ThreadStatus::dormant:
        return "dormant";
    }
    return "unknown";
}

std::string_view to_string(WaitKind kind) noexcept {
    switch (kind) {
    case WaitKind::none:
        return "none";
    case WaitKind::sleep:
        return "sleep";
    case WaitKind::semaphore:
        return "semaphore";
    case WaitKind::event_flag:
        return "event-flag";
    }
    return "unknown";
}

EeKernel::EeKernel(MachineServices& machine, EeRuntime& ee, Options options)
    : machine_(machine), ee_(ee), options_(options), handler_context_(std::make_unique<R5900Context>()) {
    handler_exec_.id = handler_fiber_id;
    handler_exec_.ctx = handler_context_.get();
}

EeKernel::~EeKernel() {
    // Fibers are destroyed suspended (fiber.h, invariant 2); none can be running here.
    ee_.set_exec(nullptr);
}

std::int32_t EeKernel::current_thread_id() const noexcept {
    return current_ != nullptr ? current_->id : 0;
}

void EeKernel::request_reschedule() noexcept {
    machine_.clock().force_safe_point();
}

R5900Context& EeKernel::boot_context() {
    if (GuestThread* boot = thread(kabi::boot_thread_id)) {
        return *boot->context;
    }
    return *handler_context_;
}

// ---- Reset.

void EeKernel::reset(std::uint32_t entry) {
    if (Fiber::current() != nullptr) {
        throw std::logic_error("EeKernel::reset on a guest fiber");
    }
    ee_.set_exec(nullptr);
    current_ = nullptr;
    last_exec_ = nullptr;
    threads_.clear();
    for (auto& q : ready_) {
        q.clear();
    }
    semaphores_.clear();
    event_flags_.clear();
    for (const auto& [id, alarm] : alarms_) {
        machine_.events().cancel(alarm.event);
    }
    alarms_.clear();
    intc_handlers_.clear();
    dmac_handlers_.clear();
    next_thread_id_ = kabi::first_thread_id;
    next_semaphore_id_ = 1;
    next_event_flag_id_ = 1;
    next_alarm_id_ = 1;
    next_intc_handler_id_ = 1;
    next_dmac_handler_id_ = 1;
    intc_head_ = 0;
    intc_tail_ = 1000;
    dmac_head_ = 0;
    dmac_tail_ = 1000;
    main_stack_base_ = 0;
    system_ = KernelSystemState{};
    reset_system_state(system_);
    handler_fiber_.reset();
    handler_queue_.clear();
    pending_alarms_.clear();
    in_handler_ = false;
    handler_busy_ = false;
    *handler_context_ = R5900Context();
    handler_exec_ = ExecState{};
    handler_exec_.id = handler_fiber_id;
    handler_exec_.ctx = handler_context_.get();
    stop_.reset();
    progress_ = false;
    last_progress_field_ = 0;
    last_resumed_ = 0;

    // HLE kernel RAM: the sentinel everywhere, then the syscall-table probe words the SDK's patch code reads.
    GuestMemory& mem = machine_.memory();
    std::vector<std::uint8_t> fill(hle_kernel_ram_end - hle_kernel_ram_base);
    for (std::size_t i = 0; i < fill.size(); i += 4) {
        std::memcpy(fill.data() + i, &hle_kernel_sentinel, 4);
    }
    mem.write_bytes(hle_kernel_ram_base, fill);
    const std::uint32_t probe[3] = {kabi::syscall_table_guest >> 16, 0, kabi::syscall_table_guest & 0xFFFFu};
    std::uint8_t probe_bytes[sizeof probe];
    std::memcpy(probe_bytes, probe, sizeof probe);
    mem.write_bytes(kabi::syscall_table_probe, probe_bytes);
    // Timer 3 as the kernel's alarm timer (kernel_abi.h): compare interrupt enabled, counting stopped.
    // Through the bus directly: without a timer device (unit-test machines) the write is simply not taken.
    std::uint8_t mode_bytes[4];
    const std::uint32_t mode = kabi::timer_mode_cmpe;
    std::memcpy(mode_bytes, &mode, sizeof mode);
    machine_.mmio().write(kabi::timer3_mode, 4, mode_bytes, MmioContext{});

    GuestThread boot;
    boot.id = kabi::boot_thread_id;
    boot.entry = entry;
    boot.context = std::make_unique<R5900Context>();
    reset_context(*boot.context, entry);
    boot.gp = gpr_u32(*boot.context, 28);
    boot.status = ThreadStatus::dormant;
    boot.kernel = this;
    boot.exec.id = boot.id;
    auto [it, inserted] = threads_.emplace(boot.id, std::move(boot));
    it->second.exec.ctx = it->second.context.get();
    enqueue_ready(it->second);
    last_exec_ = &it->second.exec;
}

// ---- The scheduler loop.

void EeKernel::run_events() {
    try {
        machine_.events().run_due(machine_.clock().now());
    } catch (const GuestStop& stop) {
        record_stop(stop.info());
    }
}

void EeKernel::step() {
    if (handler_busy_) {
        run_handler_fiber();
        return;
    }
    if (current_ != nullptr && current_->status != ThreadStatus::running) {
        current_ = nullptr; // a handler suspended, rotated or released the interrupted thread
    }
    // An interrupt is taken on behalf of the thread that holds (or is about to take) the CPU.
    if (handler_work()) {
        GuestThread* victim = current_ != nullptr ? current_ : peek_ready();
        if (can_take_interrupt(victim)) {
            if (current_ == nullptr && victim != nullptr) {
                remove_ready(*victim);
                victim->status = ThreadStatus::running;
                current_ = victim;
            }
            run_handler_fiber();
            return;
        }
    }
    GuestThread* best = peek_ready();
    if (current_ != nullptr) {
        if (best != nullptr && best->current_priority < current_->current_priority) {
            enqueue_ready(*current_, true); // preempted, not rotated: it stays first in its queue
            current_ = nullptr;
        } else {
            run_thread(*current_);
            return;
        }
    }
    if (best == nullptr) {
        idle();
        return;
    }
    remove_ready(*best);
    best->status = ThreadStatus::running;
    current_ = best;
    run_thread(*best);
}

bool EeKernel::handler_work() const {
    return machine_.interrupts().pending() || !pending_alarms_.empty() || !handler_queue_.empty();
}

bool EeKernel::can_take_interrupt(const GuestThread* thread) const {
    // With no thread to run, the kernel idles with interrupts enabled.
    return thread == nullptr || interrupts_enabled(*thread->exec.ctx);
}

Cycles EeKernel::next_deadline() const {
    const auto next = machine_.events().next_time();
    return next ? *next : std::numeric_limits<Cycles>::max();
}

void EeKernel::run_thread(GuestThread& t) {
    if (!t.fiber) {
        t.fiber = Fiber::create(&EeKernel::thread_entry, &t, options_.stack);
        t.exec.fiber = t.fiber.get();
    }
    if (last_resumed_ != t.id) {
        last_resumed_ = t.id;
        note_progress();
    }
    last_exec_ = &t.exec;
    ee_.set_exec(&t.exec);
    machine_.clock().set_deadline(next_deadline());
    switch_reason_ = SwitchReason::none;
    t.fiber->resume();
    ee_.set_exec(nullptr);
    const SwitchReason reason = switch_reason_;
    switch_reason_ = SwitchReason::none;
    switch (reason) {
    case SwitchReason::preempted:
        break; // still holds the CPU; step() decides
    case SwitchReason::blocked:
    case SwitchReason::stopped:
        if (current_ == &t) {
            current_ = nullptr;
        }
        break;
    case SwitchReason::none: // the entry returned: treat it as an exit
    case SwitchReason::exited: {
        if (current_ == &t) {
            current_ = nullptr;
        }
        t.fiber.reset();
        t.exec.fiber = nullptr;
        if (t.delete_on_exit) {
            erase_thread(t.id);
        }
        break;
    }
    case SwitchReason::handler_events:
    case SwitchReason::handler_done:
        break;
    }
}

void EeKernel::thread_entry(void* arg) {
    auto* t = static_cast<GuestThread*>(arg);
    t->kernel->ee_.run_thread(t->exec, t->id == kabi::boot_thread_id);
}

void EeKernel::idle() {
    if (deadlocked()) {
        StopInfo s;
        s.reason = StopReason::deadlock;
        s.message = "deadlock: no guest thread can run and nothing can wake one\n" + report();
        record_stop(std::move(s));
        return;
    }
    const auto next = machine_.events().next_time();
    if (!next) {
        StopInfo s;
        s.reason = StopReason::deadlock;
        s.message = "deadlock: no guest thread can run and no event is pending\n" + report();
        record_stop(std::move(s));
        return;
    }
    machine_.clock().advance_to(*next); // idle fast-forward
}

bool EeKernel::deadlocked() const {
    if (current_ != nullptr || handler_work() || !alarms_.empty()) {
        return false;
    }
    for (const auto& q : ready_) {
        if (!q.empty()) {
            return false;
        }
    }
    // Only a handler can wake a thread when none runs. One that can still fire keeps the machine waiting.
    const InterruptController& irq = machine_.interrupts();
    for (const auto& [id, h] : intc_handlers_) {
        if (h.enabled && h.cause < intc_line_count && (irq.intc_mask() & (1u << h.cause)) != 0) {
            return false;
        }
    }
    const std::uint32_t dstat = irq.dmac_stat();
    for (const auto& [id, h] : dmac_handlers_) {
        if (!h.enabled) {
            continue;
        }
        if (h.cause < dma_channel_count && (dstat & (1u << (16 + h.cause))) != 0) {
            return false;
        }
        if ((h.cause == dmac_cause_stall && (dstat & InterruptController::dstat_sim) != 0) ||
            (h.cause == dmac_cause_mfifo_empty && (dstat & InterruptController::dstat_meim) != 0) ||
            h.cause == dmac_cause_bus_error) {
            return false;
        }
    }
    return true;
}

// ---- Fiber switches.

void EeKernel::switch_out(SwitchReason reason) {
    switch_reason_ = reason;
    Fiber::yield();
}

void EeKernel::safe_point(ExecState& state) {
    if (Fiber::current() == nullptr || state.fiber == nullptr) {
        return; // off any fiber (host-side calls in tests): nothing to switch to
    }
    VirtualClock& clock = machine_.clock();
    state.ctx->cop0_count = static_cast<std::uint32_t>(clock.now());
    if (in_handler_) {
        if (clock.due()) {
            switch_out(SwitchReason::handler_events); // the scheduler runs due events and resumes us
        }
        return;
    }
    if (clock.due() || (handler_work() && interrupts_enabled(*state.ctx))) {
        switch_out(SwitchReason::preempted);
    }
}

void EeKernel::after_kernel_call(ExecState& state) {
    if (in_handler_) {
        return; // selection waits until the handler returns
    }
    safe_point(state);
}

void EeKernel::stop(StopInfo info) {
    if (Fiber::current() == nullptr) {
        throw GuestStop(std::move(info)); // the scheduler loop catches it
    }
    record_stop(std::move(info));
    switch_out(SwitchReason::stopped); // never resumed
    std::fputs("dq8: a stopped fiber was resumed\n", stderr);
    std::abort();
}

void EeKernel::record_stop(StopInfo info) {
    if (stop_) {
        return; // the first stop wins
    }
    // 0 on the scheduler's own stack (an event, a vblank check), else the fiber that stopped.
    info.thread_id = Fiber::current() == nullptr ? 0 : (in_handler_ ? handler_fiber_id : current_thread_id());
    ee_.annotate_stop(info);
    stop_ = std::move(info);
}

// ---- Interrupt handlers on the handler fiber.

void EeKernel::run_handler_fiber() {
    if (!handler_fiber_) {
        handler_fiber_ = Fiber::create(&EeKernel::handler_entry, this, options_.stack);
        handler_exec_.fiber = handler_fiber_.get();
    }
    in_handler_ = true;
    handler_busy_ = true;
    last_exec_ = &handler_exec_;
    ee_.set_exec(&handler_exec_);
    machine_.clock().set_deadline(next_deadline());
    switch_reason_ = SwitchReason::none;
    handler_fiber_->resume();
    ee_.set_exec(nullptr);
    const SwitchReason reason = switch_reason_;
    switch_reason_ = SwitchReason::none;
    if (reason == SwitchReason::handler_events) {
        return; // still busy: step() resumes it after the due events
    }
    handler_busy_ = false;
    in_handler_ = false;
    if (reason == SwitchReason::handler_done && current_ != nullptr) {
        last_exec_ = &current_->exec;
    }
}

void EeKernel::handler_entry(void* arg) {
    static_cast<EeKernel*>(arg)->handler_loop();
}

void EeKernel::handler_loop() {
    for (;;) {
        while (const auto call = next_handler_call()) {
            verify_kernel_ram("before an interrupt handler");
            R5900Context& c = *handler_context_;
            c = R5900Context();
            c.cop0_status |= status_exl; // exception level: handlers never nest
            c.pc = call->handler;
            set_gpr_lo(c, 4, call->args[0]);
            set_gpr_lo(c, 5, call->args[1]);
            set_gpr_lo(c, 6, call->args[2]);
            set_gpr_lo(c, 28, call->gp);
            set_gpr_lo(c, 29, hle_handler_stack_top);
            set_gpr_lo(c, 31, 0);
            handler_exec_.ctx = &c;
            handler_exec_.unwinding = false;
            handler_exec_.depth = 0;
            ee_.call_guest(handler_exec_, c);
            verify_kernel_ram("after an interrupt handler");
        }
        switch_out(SwitchReason::handler_done);
    }
}

void EeKernel::queue_chain(bool dmac, std::uint32_t cause) {
    const auto& handlers = dmac ? dmac_handlers_ : intc_handlers_;
    std::vector<const IrqHandler*> chain;
    for (const auto& [id, h] : handlers) {
        if (h.enabled && h.cause == cause && h.handler != 0) {
            chain.push_back(&h);
        }
    }
    std::stable_sort(chain.begin(), chain.end(),
                     [](const IrqHandler* a, const IrqHandler* b) { return a->order < b->order; });
    // Every enabled handler of the cause runs, in chain order; return values are not inspected (as upstream;
    // to be confirmed in bring-up).
    for (const IrqHandler* h : chain) {
        handler_queue_.push_back(HandlerCall{h->handler, h->gp, {cause, h->arg, 0}, dmac ? "dmac" : "intc"});
    }
}

std::optional<EeKernel::HandlerCall> EeKernel::next_handler_call() {
    InterruptController& irq = machine_.interrupts();
    while (handler_queue_.empty()) {
        // INT0 first: the lowest pending INTC cause, acknowledged before its handlers run.
        if (const std::uint32_t intc = irq.intc_pending(); intc != 0) {
            std::uint32_t cause = 0;
            while ((intc & (1u << cause)) == 0) {
                ++cause;
            }
            irq.write_intc_stat(1u << cause);
            queue_chain(false, cause);
            continue;
        }
        // Then INT1: channels, then the stall, MFIFO-empty and bus-error causes.
        if (const std::uint32_t channels = irq.dmac_pending_channels(); channels != 0) {
            std::uint32_t ch = 0;
            while ((channels & (1u << ch)) == 0) {
                ++ch;
            }
            irq.write_dmac_stat(1u << ch);
            queue_chain(true, ch);
            continue;
        }
        const std::uint32_t dstat = irq.dmac_stat();
        if ((dstat & InterruptController::dstat_sis) != 0 && (dstat & InterruptController::dstat_sim) != 0) {
            irq.write_dmac_stat(InterruptController::dstat_sis);
            queue_chain(true, dmac_cause_stall);
            continue;
        }
        if ((dstat & InterruptController::dstat_meis) != 0 &&
            (dstat & InterruptController::dstat_meim) != 0) {
            irq.write_dmac_stat(InterruptController::dstat_meis);
            queue_chain(true, dmac_cause_mfifo_empty);
            continue;
        }
        if ((dstat & InterruptController::dstat_beis) != 0) {
            irq.write_dmac_stat(InterruptController::dstat_beis);
            queue_chain(true, dmac_cause_bus_error);
            continue;
        }
        // Alarms that came due.
        if (!pending_alarms_.empty()) {
            const std::int32_t id = pending_alarms_.front();
            pending_alarms_.pop_front();
            const auto it = alarms_.find(id);
            if (it == alarms_.end()) {
                continue; // cancelled after it came due
            }
            const Alarm alarm = it->second;
            alarms_.erase(it);
            handler_queue_.push_back(
                HandlerCall{alarm.handler,
                            alarm.gp,
                            {static_cast<std::uint32_t>(alarm.id), alarm.ticks, alarm.arg},
                            "alarm"});
            continue;
        }
        return std::nullopt;
    }
    HandlerCall call = handler_queue_.front();
    handler_queue_.pop_front();
    return call;
}

// ---- Vblank checks.

void EeKernel::on_vblank_start(std::uint64_t field) {
    if (stop_) {
        return;
    }
    verify_kernel_ram("at a vblank");
    if (progress_) {
        progress_ = false;
        last_progress_field_ = field;
        return;
    }
    if (options_.no_progress_fields != 0 && field >= last_progress_field_ + options_.no_progress_fields) {
        StopInfo s;
        s.reason = StopReason::no_progress;
        s.message = "no progress: no guest thread ran a kernel call or switched for " +
                    std::to_string(field - last_progress_field_) + " fields\n" + report();
        record_stop(std::move(s));
    }
}

// ---- Kernel RAM.

void EeKernel::check_kernel_ram_target(std::uint32_t address, std::uint32_t size, std::string_view what) {
    const std::uint32_t phys = address & 0x1FFFFFFFu;
    const std::uint64_t end = std::uint64_t{phys} + (size == 0 ? 1 : size);
    if (phys < hle_kernel_ram_end && end > hle_kernel_ram_base) {
        StopInfo s;
        s.reason = StopReason::kernel_ram_collision;
        s.address = address;
        s.message = std::string(what) + " targets " + hex32(address) + " (+" + std::to_string(size) +
                    " bytes), inside the HLE kernel's reserved RAM [" + hex32(hle_kernel_ram_base) + ", " +
                    hex32(hle_kernel_ram_end) + ")";
        stop(std::move(s));
    }
}

void EeKernel::verify_kernel_ram(std::string_view when) {
    if (stop_) {
        return;
    }
    const std::uint8_t* ram = machine_.memory().rdram();
    for (std::uint32_t a = hle_kernel_ram_base; a < hle_handler_stack_base; a += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, ram + a, sizeof word);
        if (word != hle_kernel_sentinel) {
            StopInfo s;
            s.reason = StopReason::kernel_ram_collision;
            s.address = a;
            s.message = "the HLE kernel's reserved RAM was overwritten at " + hex32(a) + " (found " +
                        hex32(word) + ", " + std::string(when) +
                        "): a guest write, or an interrupt handler's stack overflow";
            if (Fiber::current() != nullptr) {
                stop(std::move(s));
            }
            record_stop(std::move(s));
            return;
        }
    }
}

// ---- Report.

std::string EeKernel::report() const {
    std::string out;
    char line[256];
    std::snprintf(line, sizeof line, "  cycles %llu, current thread %d%s\n",
                  static_cast<unsigned long long>(machine_.clock().now()),
                  static_cast<int>(current_ != nullptr ? current_->id : 0),
                  in_handler_ ? " (in a handler)" : "");
    out += line;
    for (const auto& [id, t] : threads_) {
        std::snprintf(line, sizeof line,
                      "  thread %d: %s prio %d entry %s pc %s wait %s %d suspend %u wakeup %u\n",
                      static_cast<int>(id), std::string(to_string(t.status)).c_str(),
                      static_cast<int>(t.current_priority), hex32(t.entry).c_str(),
                      hex32(t.exec.ctx != nullptr ? t.exec.ctx->pc : 0).c_str(),
                      std::string(to_string(t.wait.kind)).c_str(), static_cast<int>(t.wait.object),
                      static_cast<unsigned>(t.suspend_count), static_cast<unsigned>(t.wakeup_count));
        out += line;
    }
    for (const auto& [id, s] : semaphores_) {
        std::snprintf(line, sizeof line, "  semaphore %d: count %d/%d, %zu waiting\n", static_cast<int>(id),
                      static_cast<int>(s.count), static_cast<int>(s.max_count), s.waiters.size());
        out += line;
    }
    for (const auto& [id, f] : event_flags_) {
        std::snprintf(line, sizeof line, "  event flag %d: bits %s, %zu waiting\n", static_cast<int>(id),
                      hex32(f.bits).c_str(), f.waiters.size());
        out += line;
    }
    for (const auto& [id, a] : alarms_) {
        std::snprintf(line, sizeof line, "  alarm %d: handler %s\n", static_cast<int>(id),
                      hex32(a.handler).c_str());
        out += line;
    }
    const InterruptController& irq = machine_.interrupts();
    std::snprintf(line, sizeof line, "  INTC stat %s mask %s, D_STAT %s\n", hex32(irq.intc_stat()).c_str(),
                  hex32(irq.intc_mask()).c_str(), hex32(irq.dmac_stat()).c_str());
    out += line;
    for (const auto* table : {&intc_handlers_, &dmac_handlers_}) {
        for (const auto& [id, h] : *table) {
            std::snprintf(line, sizeof line, "  %s handler %d: cause %u -> %s%s\n",
                          table == &intc_handlers_ ? "INTC" : "DMAC", static_cast<int>(id),
                          static_cast<unsigned>(h.cause), hex32(h.handler).c_str(),
                          h.enabled ? "" : " (disabled)");
            out += line;
        }
    }
    for (const EventQueue::PendingEvent& e : machine_.events().pending()) {
        std::snprintf(line, sizeof line, "  event %s at %llu\n", std::string(e.tag).c_str(),
                      static_cast<unsigned long long>(e.at));
        out += line;
    }
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    return out;
}

} // namespace dq8::rt
