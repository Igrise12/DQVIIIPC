// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors: the kernel-object core of
// ps2xRuntime/src/lib/Kernel/EeScheduler.cpp (threads, ready queues, semaphores, event flags, alarms, INTC
// and DMAC handler chains). What changed: blocking is a fiber switch instead of a thrown transfer; there are
// no host threads, mutexes or wall-clock deadlines; alarms tick on the virtual hblank clock; interrupts come
// from the InterruptController and are taken at safe points on one handler fiber; vblanks belong to the
// machine.
#pragma once

#include "dq8/abi/ps2recomp/r5900_context.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/kernel/fiber.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/machine/clock.h"
#include "runtime/machine/events.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dq8::rt {

class MachineServices;
class EeRuntime;
struct MachineConfig;
class EeKernel;

// What one fiber executes with: a guest thread's, or the interrupt handler fiber's. EeRuntime reads it on
// every call generated code makes on that fiber.
struct ExecState {
    R5900Context* ctx = nullptr; // the active context: the thread's own, or a nested guest call's
    bool unwinding = false;      // a jump or an unexpected return is unwinding the host stack to a run loop
    std::uint32_t depth = 0;     // dispatched calls nested on this fiber's stack
    Fiber* fiber = nullptr;
    std::int32_t id = 0; // the thread id, or handler_fiber_id
    // Kernel call numbers whose SetSyscall guest handler is running on this fiber (a nested call of the same
    // number reaches the native service), and the contexts of those nested calls (owned here, not by the
    // fiber's frames, so an abandoned fiber leaks nothing).
    std::vector<std::int32_t> active_syscall_handlers;
    std::vector<std::unique_ptr<R5900Context>> nested_contexts;
};

inline constexpr std::int32_t handler_fiber_id = -1;

enum class ThreadStatus : std::uint8_t { running, ready, waiting, waiting_suspended, suspended, dormant };
enum class WaitKind : std::uint8_t { none, sleep, semaphore, event_flag };

std::string_view to_string(ThreadStatus status) noexcept;
std::string_view to_string(WaitKind kind) noexcept;

struct WaitState {
    WaitKind kind = WaitKind::none;
    std::int32_t object = 0;          // semaphore or event-flag id
    std::uint32_t bits = 0;           // event flag: requested bits
    std::uint32_t mode = 0;           // event flag: WEF mode
    std::uint32_t result_address = 0; // event flag: where the observed bits go (0 = nowhere)
};

struct ThreadParams {
    std::uint32_t entry = 0;
    std::uint32_t stack = 0; // lowest address of the stack
    std::uint32_t stack_size = 0;
    std::uint32_t gp = 0;
    std::int32_t priority = 0;
    std::uint32_t attr = 0;
    std::uint32_t option = 0;
};

struct GuestThread {
    std::int32_t id = 0;
    std::uint32_t entry = 0;
    std::uint32_t stack = 0;
    std::uint32_t stack_size = 0;
    std::uint32_t gp = 0;
    std::uint32_t attr = 0;
    std::uint32_t option = 0;
    std::int32_t initial_priority = 0;
    std::int32_t current_priority = 0;
    ThreadStatus status = ThreadStatus::dormant;
    WaitState wait;
    std::uint32_t suspend_count = 0;
    std::uint32_t wakeup_count = 0;
    std::unique_ptr<R5900Context> context; // the thread's own register context
    ExecState exec;
    std::unique_ptr<Fiber> fiber; // created when the thread first runs after a start
    bool delete_on_exit = false;
    EeKernel* kernel = nullptr; // for the fiber entry
};

struct Semaphore {
    std::int32_t id = 0;
    std::int32_t count = 0;
    std::int32_t max_count = 0;
    std::int32_t init_count = 0;
    std::uint32_t attr = 0;
    std::uint32_t option = 0;
    std::deque<std::int32_t> waiters; // FIFO
};

struct EventFlag {
    std::int32_t id = 0;
    std::uint32_t attr = 0;
    std::uint32_t option = 0;
    std::uint32_t init_bits = 0;
    std::uint32_t bits = 0;
    std::deque<std::int32_t> waiters; // FIFO
};

struct Alarm {
    std::int32_t id = 0;
    std::uint32_t ticks = 0;
    std::uint32_t handler = 0;
    std::uint32_t arg = 0;
    std::uint32_t gp = 0;
    EventId event = 0;
};

// One registered INTC or DMAC handler.
struct IrqHandler {
    std::int32_t id = 0;
    std::uint32_t cause = 0;
    std::uint32_t handler = 0;
    std::uint32_t arg = 0;
    std::uint32_t gp = 0;
    bool enabled = true;
    std::int32_t order = 0; // chain position: AddIntcHandler's `next` 0 puts it first, anything else last
};

// What the system calls keep between calls (services_system.cpp): the heap SetupHeap declared and the OSD
// configuration words.
struct KernelSystemState {
    std::uint32_t heap_start = 0;
    std::uint32_t heap_end = 0;    // 0 until SetupHeap
    std::uint32_t osd_config = 0;  // ConfigParam
    std::uint32_t osd_config2 = 0; // Config2Param
};

// The EE kernel: threads on fibers, the scheduler loop, safe points, interrupt delivery on the handler fiber,
// and the kernel objects the services in services_*.cpp operate on. Owned by the Machine; one machine thread.
class EeKernel final : public Scheduler {
  public:
    struct Options {
        std::uint32_t max_call_depth = 4096;
        std::uint32_t no_progress_fields = 0; // 0: the watchdog is off
        FiberStackSpec stack{};
        // One VU0 register file for every thread (--vu0-registers shared), as on the console, instead of a
        // VU0 block per thread context (the default, as PS2Recomp keeps them).
        bool vu0_shared = false;
    };

    EeKernel(MachineServices& machine, EeRuntime& ee, Options options);
    ~EeKernel() override;
    EeKernel(const EeKernel&) = delete;
    EeKernel& operator=(const EeKernel&) = delete;

    // ---- Scheduler.
    std::int32_t current_thread_id() const noexcept override;
    bool in_handler() const noexcept override { return in_handler_; }
    void request_reschedule() noexcept override;
    [[noreturn]] void stop(StopInfo info) override;

    // ---- Machine side (scheduler stack).
    // Drops every object, fills the HLE kernel RAM with its sentinel, seeds the syscall-table probe words,
    // and creates the boot thread (id 1, priority 0) at `entry` with the power-on context.
    void reset(std::uint32_t entry);
    // The scheduler loop: runs events, interrupt handlers and guest threads until the machine stops or
    // `slice_done` returns true (checked between switches). Resumable.
    template <typename Done> void run(Done&& slice_done) {
        while (!stop_) {
            run_events();
            if (stop_ || slice_done()) {
                return;
            }
            step();
        }
    }
    bool stopped() const noexcept { return stop_.has_value(); }
    const std::optional<StopInfo>& stop_info() const noexcept { return stop_; }
    // Records a stop raised on the scheduler stack (an event, a vblank check, the watchdog).
    void record_stop(StopInfo info);
    // At each vblank start, from the machine: the RAM sentinel and the no-progress watchdog.
    void on_vblank_start(std::uint64_t field);
    // The execution state of the thread or handler that ran last (for the stop report), or null.
    const ExecState* last_exec() const noexcept { return last_exec_; }
    R5900Context& boot_context();
    // The VU0 registers EE code sees now, which VIF0's MSCAL runs on: the shared file with
    // Options::vu0_shared, else the running (or interrupted) thread's, else those of the thread that ran
    // last.
    R5900Context& vu0_view() noexcept;

    // ---- Safe points (on a fiber; off any fiber they do nothing).
    // eeCheckpointDue, a dispatched branch, a run loop: switches when the clock is due or an interrupt can be
    // taken. Returns normally.
    void safe_point(ExecState& state);
    // The end of a kernel call: switches when a reschedule was requested (never in interrupt context).
    void after_kernel_call(ExecState& state);
    // A guest thread made progress (a kernel call, a switch): feeds the watchdog.
    void note_progress() noexcept { progress_ = true; }

    // ---- Threads (adapted from EeScheduler). Results are kabi codes or ids.
    std::int32_t create_thread(const ThreadParams& params);
    std::int32_t delete_thread(std::int32_t id);
    std::int32_t start_thread(std::int32_t id, std::uint32_t arg, const R5900Context& caller);
    [[noreturn]] void exit_current(bool delete_thread);
    std::int32_t terminate_thread(std::int32_t id);
    std::int32_t suspend_thread(std::int32_t id);
    std::int32_t resume_thread(std::int32_t id);
    // Blocks the calling thread until WakeupThread unless a wakeup is pending; returns the result for $v0.
    std::int32_t sleep_current();
    std::int32_t wakeup_thread(std::int32_t id);
    std::int32_t cancel_wakeup(std::int32_t id);
    std::int32_t change_priority(std::int32_t id, std::int32_t priority, std::int32_t& old_priority);
    std::int32_t rotate_ready_queue(std::int32_t priority);
    std::int32_t release_wait(std::int32_t id);
    // SetupThread's record of the boot thread's stack and global pointer (ReferThreadStatus reports it).
    void setup_current(std::uint32_t stack, std::uint32_t stack_size, std::uint32_t gp);
    GuestThread* thread(std::int32_t id);
    const GuestThread* thread(std::int32_t id) const;
    GuestThread* current_thread() noexcept { return current_; }
    std::uint32_t main_stack_base() const noexcept { return main_stack_base_; }
    KernelSystemState& system_state() noexcept { return system_; }

    // ---- Semaphores.
    std::int32_t create_semaphore(std::int32_t init_count, std::int32_t max_count, std::uint32_t attr,
                                  std::uint32_t option);
    std::int32_t delete_semaphore(std::int32_t id);
    std::int32_t signal_semaphore(std::int32_t id);
    std::int32_t poll_semaphore(std::int32_t id);
    // Takes a count or blocks; returns the result for $v0 (the id, or an error).
    std::int32_t wait_semaphore(std::int32_t id);
    const Semaphore* semaphore(std::int32_t id) const;

    // ---- Event flags.
    std::int32_t create_event_flag(std::uint32_t init_bits, std::uint32_t attr, std::uint32_t option);
    std::int32_t delete_event_flag(std::int32_t id);
    std::int32_t set_event_flag(std::int32_t id, std::uint32_t bits);
    std::int32_t clear_event_flag(std::int32_t id, std::uint32_t mask);
    std::int32_t poll_event_flag(std::int32_t id, std::uint32_t bits, std::uint32_t mode,
                                 std::uint32_t& observed);
    // Completes at once or blocks; writes the observed bits to `result_address` (when non-zero) on
    // completion.
    std::int32_t wait_event_flag(std::int32_t id, std::uint32_t bits, std::uint32_t mode,
                                 std::uint32_t result_address);
    const EventFlag* event_flag(std::int32_t id) const;

    // ---- Alarms: `ticks` hblanks from now, the handler runs on the handler fiber.
    std::int32_t set_alarm(std::uint32_t ticks, std::uint32_t handler, std::uint32_t arg, std::uint32_t gp);
    std::int32_t cancel_alarm(std::int32_t id);
    std::size_t alarm_count() const noexcept { return alarms_.size(); }

    // ---- INTC (dmac = false) and DMAC (dmac = true) handler chains.
    std::int32_t add_handler(bool dmac, std::uint32_t cause, std::uint32_t handler, std::int32_t next,
                             std::uint32_t arg, std::uint32_t gp);
    std::int32_t remove_handler(bool dmac, std::uint32_t cause, std::int32_t id);
    std::int32_t set_handler_enabled(bool dmac, std::int32_t id, bool enabled);

    // ---- Kernel RAM.
    // Stops with kernel_ram_collision when a write or code range touches the HLE kernel's reserved RAM.
    void check_kernel_ram_target(std::uint32_t address, std::uint32_t size, std::string_view what);
    // Stops when the reserved part of the HLE kernel RAM no longer holds the sentinel.
    void verify_kernel_ram(std::string_view when);

    // A multi-line report of every thread, wait object, alarm, handler and pending event (deadlock and
    // watchdog stops carry it).
    std::string report() const;

  private:
    enum class SwitchReason : std::uint8_t {
        none,
        preempted,
        blocked,
        exited,
        stopped,
        handler_events,
        handler_done
    };
    struct HandlerCall {
        std::uint32_t handler = 0;
        std::uint32_t gp = 0;
        std::array<std::uint32_t, 3> args{};
        std::string_view kind; // "intc", "dmac", "alarm"
    };

    void step();
    void run_events();
    void run_thread(GuestThread& thread);
    void run_handler_fiber();
    bool handler_work() const;
    bool can_take_interrupt(const GuestThread* thread) const;
    void idle();
    bool deadlocked() const;
    void switch_out(SwitchReason reason);
    Cycles next_deadline() const;

    static void thread_entry(void* arg);
    static void handler_entry(void* arg);
    void handler_loop();
    std::optional<HandlerCall> next_handler_call();
    void queue_chain(bool dmac, std::uint32_t cause);

    // Ready queues and waits.
    void enqueue_ready(GuestThread& thread, bool front = false);
    void remove_ready(GuestThread& thread);
    GuestThread* peek_ready();
    void make_ready(GuestThread& thread, std::int32_t result);
    void remove_from_wait_object(GuestThread& thread);
    void make_dormant(GuestThread& thread);
    void request_preemption_if_higher(const GuestThread& ready);
    // Blocks the running thread with `wait` (stops in interrupt context or off any thread).
    void block_current(WaitState wait, std::string_view call);
    GuestThread& running_thread(std::string_view call);
    void finish_event_waiters(EventFlag& flag);
    void write_guest_u32(std::uint32_t address, std::uint32_t value);
    void erase_thread(std::int32_t id);
    std::uint64_t lines_started(Cycles t) const noexcept;

    MachineServices& machine_;
    EeRuntime& ee_;
    Options options_;

    std::map<std::int32_t, GuestThread> threads_;
    std::array<std::deque<std::int32_t>, 128> ready_;
    std::map<std::int32_t, Semaphore> semaphores_;
    std::map<std::int32_t, EventFlag> event_flags_;
    std::map<std::int32_t, Alarm> alarms_;
    std::map<std::int32_t, IrqHandler> intc_handlers_;
    std::map<std::int32_t, IrqHandler> dmac_handlers_;
    std::int32_t next_thread_id_ = 2;
    std::int32_t next_semaphore_id_ = 1;
    std::int32_t next_event_flag_id_ = 1;
    std::int32_t next_alarm_id_ = 1;
    std::int32_t next_intc_handler_id_ = 1;
    std::int32_t next_dmac_handler_id_ = 1;
    std::int32_t intc_head_ = 0;
    std::int32_t intc_tail_ = 1000;
    std::int32_t dmac_head_ = 0;
    std::int32_t dmac_tail_ = 1000;
    std::uint32_t main_stack_base_ = 0;
    KernelSystemState system_;

    GuestThread* current_ = nullptr; // running, or preempted and still holding the CPU
    const ExecState* last_exec_ = nullptr;
    SwitchReason switch_reason_ = SwitchReason::none;
    std::int32_t last_resumed_ = 0;

    // The handler fiber and what it runs.
    std::unique_ptr<Fiber> handler_fiber_;
    ExecState handler_exec_;
    std::unique_ptr<R5900Context> handler_context_;
    // The VU0 block of the thread that ran last (every switch-out copies it here); with vu0_shared the one
    // register file every thread and handler works on.
    std::unique_ptr<R5900Context> vu0_file_;
    bool in_handler_ = false;
    bool handler_busy_ = false; // the handler fiber is suspended in the middle of its work
    std::deque<HandlerCall> handler_queue_;
    std::deque<std::int32_t> pending_alarms_;

    std::optional<StopInfo> stop_;
    bool progress_ = false;
    std::uint64_t last_progress_field_ = 0;
};

} // namespace dq8::rt
