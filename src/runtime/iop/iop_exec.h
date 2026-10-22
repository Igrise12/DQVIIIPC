// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/ee/guest_stop.h"
#include "runtime/iop/cpu/iop_cpu.h"
#include "runtime/iop/hw/iop_hardware.h"
#include "runtime/iop/kernel/iop_kernel.h"
#include "runtime/iop/loader/iop_libraries.h"
#include "runtime/machine/clock.h"
#include "runtime/machine/events.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The interpreted IOP (Docs/milestones/M4-playable.md, session A; ADR-0014): the R3000A core, the IOP bus,
// the HLE kernel and its libraries, and the slice scheduler that runs them in virtual time.
//
// Time. The IOP keeps its own clock (EE cycles; one instruction is CostModel::iop_instruction). Its code runs
// only from its slice event ("iop_slice"): a slice starts at the event's time and runs contexts until the
// next machine event or CostModel::iop_slice cycles, whichever comes first, then schedules the next slice if
// anything can still run. A context that can run is the interrupt context (a queued handler, when the running
// thread has interrupts enabled) or the ready thread of the highest priority. An idle IOP schedules nothing
// and costs nothing: a wake (an RPC request, a timer, a device interrupt, a time wait ending) moves the IOP
// clock to the wake's time and schedules a slice. With no interpreted module loaded nothing here ever
// schedules an event, so the M3 path is unchanged.
//
// Every guest-visible effect is deterministic: no wall time, ids count up, containers are ordered by keys.
namespace dq8::rt {

class HleCall;
class IopMemory;
class IopRpcLayer;
class MachineServices;
class SifLink;
struct CostModel;
struct HleFunction;

class IopExec final : public IIopHardwareOwner {
  public:
    struct Module {
        std::string name;
        std::int32_t id = 0;
        std::uint32_t base = 0;
        std::uint32_t size = 0;
        std::uint32_t entry = 0;
        std::uint32_t gp = 0;
        std::uint32_t text_size = 0;
        bool started = false;
        bool resident = true;
        std::int32_t start_result = 0;
        std::uint32_t hle_imports = 0;
        std::uint32_t guest_imports = 0;
        std::uint32_t unimplemented_imports = 0;
    };
    // Called once a module's start function returned.
    using StartDone = std::function<void(const Module& module, std::int32_t start_result)>;

    // Slots 0-3 of the HLE page are return traps; HLE functions follow, then stop traps for imports nobody
    // implements.
    enum class SlotKind : std::uint8_t {
        thread_exit,
        start_return,
        call_return,
        irq_return,
        hle,
        unimplemented
    };

    IopExec(MachineServices& machine, IopMemory& ram, SifLink& link, IopRpcLayer& rpc,
            std::vector<IopDeviceMapping> devices, bool strict_imports);
    ~IopExec() override;
    IopExec(const IopExec&) = delete;
    IopExec& operator=(const IopExec&) = delete;

    void connect_devices();
    // Power-on and IOP reboot: every module, thread, server and pending event goes.
    void reset();

    // Loads an IRX image (the loadfile request for a module the profile marks "interpret"), links it and runs
    // its start function on a new IOP thread; `done` runs when start returns. A bad file, a missing library
    // or (with strict imports) an unimplemented import stops the guest here.
    void load_module(std::string name, std::int32_t id, std::span<const std::uint8_t> file,
                     std::string_view path, std::span<const std::uint8_t> args, StartDone done);
    // An RPC end packet from the EE for one of the IOP's client requests.
    void on_rpc_end(std::span<const std::uint8_t> packet);

    bool has_modules() const noexcept { return !modules_.empty(); }
    const std::vector<Module>& modules() const noexcept { return modules_; }
    IopHardware& hardware() noexcept { return *hw_; }
    Cycles now() const noexcept { return now_; }
    std::uint64_t instructions() const noexcept;
    std::uint64_t hle_calls() const noexcept { return hle_calls_; }
    std::uint64_t slices() const noexcept { return slices_; }

    // IIopHardwareOwner.
    Cycles hw_now() const override { return now_; }
    void hw_interrupt(Cycles at) override;
    void hw_poll(std::uint32_t pc, std::uint32_t address, std::uint32_t value) override;
    void hw_activity() override;
    [[noreturn]] void hw_stop(std::string message, std::uint32_t pc, std::uint32_t address) override;

    // ---- The kernel, as the HLE libraries use it ---------------------------------------------------------

    MachineServices& machine() noexcept { return machine_; }
    IopMemory& ram() noexcept { return ram_; }
    SifLink& link() noexcept { return link_; }
    IopRpcLayer& rpc() noexcept { return rpc_; }
    IopLibraries& libraries() noexcept { return libraries_; }
    const CostModel& cost() const noexcept;
    void charge(Cycles cycles) noexcept { now_ += cycles; }
    // The IOP system clock (36.864 MHz ticks since this IOP boot).
    std::uint64_t sysclock() const noexcept;
    static Cycles cycles_of_ticks(std::uint64_t ticks) noexcept { return ticks * ee_cycles_per_iop_cycle; }

    // "LIBSD+0x00001234" for an address inside a module, else the address.
    std::string location(std::uint32_t pc) const;
    std::string module_at(std::uint32_t pc) const;
    [[noreturn]] void stop(StopReason reason, const std::string& message, std::uint32_t pc,
                           std::uint32_t address = 0);

    // Memory: sysmem's allocator (modes 0 lowest, 1 highest, 2 at an address), in 256-byte units.
    std::uint32_t alloc(std::uint32_t mode, std::uint32_t size, std::uint32_t address);
    bool free(std::uint32_t address);

    // Threads.
    IopThread* thread(std::int32_t id);
    IopThread* current_thread();
    bool in_interrupt() const noexcept { return in_irq_; }
    std::int32_t create_thread(std::uint32_t attr, std::uint32_t option, std::uint32_t entry,
                               std::uint32_t stack_size, std::int32_t priority, std::uint32_t gp,
                               std::string module);
    void start_thread(IopThread& t, std::uint32_t arg);
    void delete_thread(IopThread& t);
    void terminate_thread(IopThread& t);
    void change_priority(IopThread& t, std::int32_t priority);
    // The calling thread starts waiting; its $v0 is set when it wakes.
    void block(IopThread& t, IopWait wait, std::uint32_t id);
    void delay(IopThread& t, Cycles cycles);
    // A waiting thread becomes ready with $v0 = result (it leaves any wait list).
    void wake(IopThread& t, std::int32_t result);
    void write_thread_status(const IopThread& t, std::uint32_t info, HleCall& call);

    // Semaphores and event flags.
    std::map<std::int32_t, IopSema>& semas() noexcept { return semas_; }
    std::map<std::int32_t, IopEventFlag>& event_flags() noexcept { return event_flags_; }
    std::int32_t next_object_id() noexcept { return next_object_id_++; }
    // Wakes the waiters of `flag` whose condition holds now.
    void check_event_flag(IopEventFlag& flag);
    // Applies WaitEventFlag's clear mode after a match.
    static void consume_event_flag(IopEventFlag& flag, std::uint32_t bits, std::uint32_t mode) noexcept;
    static bool event_flag_matches(std::uint32_t flag, std::uint32_t bits, std::uint32_t mode) noexcept;

    // Interrupts (intrman) and interrupt-context work.
    std::map<std::uint32_t, IopIntrHandler>& intr_handlers() noexcept { return intr_handlers_; }
    // Queues work for the interrupt context and wakes the IOP at `at` if it idles.
    void queue_irq(const IopIrqJob& job, Cycles at);

    // Alarms (thbase) and hardware timers (timrman).
    std::uint32_t set_alarm(std::uint64_t ticks, std::uint32_t handler, std::uint32_t common,
                            std::uint32_t gp);
    bool cancel_alarm(std::uint32_t handler, std::uint32_t common);
    std::array<IopHardTimer, 6>& timers() noexcept { return timers_; }
    void schedule_timer(std::size_t index, Cycles from);
    void cancel_timer(std::size_t index);
    // EE cycles for `ticks` counts of a timer's source and prescale (rounded up).
    Cycles timer_cycles(const IopHardTimer& t, std::uint64_t ticks) const noexcept;

    // SIF RPC (sifcmd) and SIF DMA (sifman).
    std::int32_t register_server(const IopGuestServer& server);
    void remove_server(std::uint32_t data);
    std::map<std::uint32_t, IopRpcQueue>& rpc_queues() noexcept { return rpc_queues_; }
    std::map<std::uint32_t, IopGuestServer>& guest_servers() noexcept { return guest_servers_; }
    // sceSifRpcLoop: serve the queue forever on `t` (returns into guest code or blocks).
    void rpc_loop(IopThread& t, std::uint32_t queue);
    // A request from the EE for a guest server (from IopRpcLayer, outside any slice).
    void enqueue_request(const IopGuestRequest& request);
    void client_bind(HleCall& call, std::uint32_t client, std::uint32_t sid, std::uint32_t mode);
    void client_call(HleCall& call, std::uint32_t client, std::uint32_t fno, std::uint32_t mode,
                     std::uint32_t send, std::uint32_t send_size, std::uint32_t recv, std::uint32_t recv_size,
                     std::uint32_t end_function, std::uint32_t end_param);
    std::uint32_t sif_dma(HleCall& call, std::uint32_t descriptors, std::uint32_t count);
    std::int32_t sif_dma_status(std::uint32_t id) const;

    // Module bookkeeping for loadcore.
    const Module* module_containing(std::uint32_t address) const;

    // printf and Kprintf output, line by line into the "iop" trace category.
    void print(std::string_view text);

  private:
    struct Slot {
        SlotKind kind = SlotKind::hle;
        const HleFunction* function = nullptr;
        std::string library; // unimplemented: the import
        std::uint16_t ordinal = 0;
    };
    struct PendingStart {
        std::size_t module = 0;
        std::int32_t thread = 0;
        StartDone done;
    };

    std::uint32_t slot_address(std::uint32_t slot) const noexcept {
        return iop_map::hle_page + slot * iop_map::hle_slot_size;
    }
    std::uint32_t unimplemented_slot(const std::string& library, std::uint16_t ordinal);
    void link_module(Module& m);

    // Scheduling.
    void wake_iop(Cycles at);
    void schedule_slice(Cycles at);
    void run_slice(Cycles when);
    bool dispatch();
    bool runnable() const;
    bool irq_ready() const;
    void collect_interrupts();
    void begin_irq_job();
    IopThread* select_thread();
    void make_ready(IopThread& t, bool front = false);
    void unready(IopThread& t);
    void refresh_limit();
    void run_context(IopCpuState& cpu, IopThread* t);
    void handle_trap(IopCpuState& cpu, IopThread* t, std::uint32_t slot);
    [[noreturn]] void fault(const IopCpuState& cpu, IopThread* t, const IopCpuStop& stop);
    void finish_irq_job();
    void finish_module_start(IopThread& t);
    void thread_exit(IopThread& t);
    void serve_next(IopThread& t, IopRpcQueue& q);
    void complete_request(IopThread& t);
    void remove_waiter(IopThread& t);
    void erase_thread(std::int32_t id);
    std::string context_name(const IopThread* t) const;
    void alarm_due(std::uint32_t id, Cycles when);
    void timer_due(std::size_t index, Cycles when);
    void ensure_irq_stack();

    MachineServices& machine_;
    IopMemory& ram_;
    SifLink& link_;
    IopRpcLayer& rpc_;
    bool strict_imports_;
    std::unique_ptr<IopHardware> hw_;
    std::unique_ptr<IopCpu> cpu_;
    IopLibraries libraries_;
    std::vector<Slot> slots_;
    std::size_t fixed_slots_ = 0; // the return traps and the HLE functions; stop traps come after

    // Time and slices.
    Cycles now_ = 0;
    Cycles boot_ = 0;
    Cycles limit_ = 0;     // the slice ends here
    Cycles run_limit_ = 0; // the current CPU run ends here (lowered to break out early)
    bool in_slice_ = false;
    EventId slice_event_ = 0;
    std::uint64_t hle_calls_ = 0;
    std::uint64_t slices_ = 0;
    // The IOP spin detector: identical reads of one device register from one pc.
    std::uint32_t poll_pc_ = 0;
    std::uint32_t poll_address_ = 0;
    std::uint32_t poll_value_ = 0;
    std::uint32_t poll_count_ = 0;

    // Modules.
    std::vector<Module> modules_;
    std::vector<PendingStart> starts_;

    // Threads.
    std::map<std::int32_t, IopThread> threads_;
    std::array<std::deque<std::int32_t>, 128> ready_{};
    std::int32_t current_ = 0;
    std::int32_t next_thread_id_ = 1;

    // Interrupts.
    std::map<std::uint32_t, IopIntrHandler> intr_handlers_;
    std::deque<IopIrqJob> irq_jobs_;
    bool in_irq_ = false;
    IopIrqJob irq_job_;
    IopCpuState irq_cpu_;
    std::uint32_t irq_stack_ = 0; // the top of the interrupt stack, 0 until the first module loads

    // Kernel objects.
    std::map<std::int32_t, IopSema> semas_;
    std::map<std::int32_t, IopEventFlag> event_flags_;
    std::int32_t next_object_id_ = 1;
    std::map<std::uint32_t, IopAlarm> alarms_;
    std::uint32_t next_alarm_id_ = 1;
    std::array<IopHardTimer, 6> timers_{};

    // SIF.
    std::map<std::uint32_t, IopGuestServer> guest_servers_; // by server structure address
    std::map<std::uint32_t, IopRpcQueue> rpc_queues_;
    std::map<std::uint32_t, IopClientOp> clients_; // by client structure address
    std::uint32_t next_rpc_id_ = 0;
    std::map<std::uint32_t, bool> sif_dma_done_; // transfer id -> delivered
    std::uint32_t next_sif_dma_ = 1;

    std::string print_line_;
};

} // namespace dq8::rt
