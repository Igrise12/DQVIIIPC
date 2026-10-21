// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/abi/game_module.h"
#include "dq8/abi/ps2recomp/ps2_runtime.h"
#include "runtime/ee/dispatcher.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/ee/guest_stop.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// The EE side of the host runtime: the dq8_runtime_api table that generated code reaches through its
// PS2Runtime adapter, the dispatch contract for every jal/jalr/jr, kernel calls through KernelServices, and
// the run loops guest fibers execute. The Machine (machine.h) owns it and the EeKernel that schedules the
// fibers. Behaviour follows PS2Runtime::dispatchGuestBranch at eb8688b except where src/runtime/README.md
// says otherwise; since M3 every switch happens inside a safe point and nothing unwinds for preemption
// (ADR-0007).
namespace dq8::rt {

class MachineServices;
class KernelServices;
class EeKernel;
struct ExecState;

// The M2 host options, kept for dq8-game's command line; the Machine takes them through MachineConfig.
struct RunOptions {
    bool stop_at_first_syscall = false;  // stop at a kernel call even when a service implements it
    std::uint64_t max_dispatches = 0;    // 0: no limit. Counts run-loop entries and every dispatched branch
    std::uint32_t max_call_depth = 4096; // nested guest calls per fiber before the guard stops the guest
    bool trace_dispatch = false;         // one trace line per dispatch
};

struct RunResult {
    StopInfo stop;
    std::uint64_t dispatches = 0;
};

class EeRuntime final : public IGuestCalls {
  public:
    using TraceSink = std::function<void(std::string_view line)>;

    // The references must outlive the runtime.
    EeRuntime(GuestMemory& memory, const Dispatcher& dispatcher, MachineServices& machine,
              KernelServices& kernel_services);
    ~EeRuntime();
    EeRuntime(const EeRuntime&) = delete;
    EeRuntime& operator=(const EeRuntime&) = delete;

    // Wiring and options, from the Machine.
    void attach_kernel(EeKernel& kernel) noexcept { kernel_ = &kernel; }
    void set_options(const RunOptions& options) noexcept { options_ = options; }
    const RunOptions& options() const noexcept { return options_; }
    void set_max_dispatches(std::uint64_t max) noexcept { options_.max_dispatches = max; }
    std::uint64_t dispatches() const noexcept { return dispatches_; }
    // Trace lines go here (default: stderr).
    void set_trace_sink(TraceSink sink) { trace_ = std::move(sink); }

    // The context of the thread or handler that is running, or that ran last (after a stop: the one that
    // stopped); before boot, a power-on context.
    R5900Context& context() noexcept;
    GuestMemory& memory() noexcept { return memory_; }
    MachineServices& machine() noexcept { return machine_; }
    const Dispatcher& dispatcher() const noexcept { return dispatcher_; }
    // What generated code receives as `runtime`.
    PS2Runtime* adapter() noexcept { return &adapter_; }
    bool has_function(std::uint32_t address) const noexcept { return dispatcher_.lookup(address) != nullptr; }

    // ---- Execution, from the EeKernel.
    // The fiber state calls are made on (set around every resume; null on the scheduler stack).
    void set_exec(ExecState* state) noexcept { exec_ = state; }
    ExecState* exec() const noexcept { return exec_; }
    // A guest thread's run loop: dispatches from ctx->pc until the thread exits or the machine stops (pc 0
    // ends the program for the boot thread and exits any other thread). Never returns.
    [[noreturn]] void run_thread(ExecState& state, bool boot_thread);
    // A nested guest call on the current fiber: runs `ctx` from its pc until it returns to pc 0 (interrupt
    // handlers, SetSyscall handlers). `state.ctx` points at `ctx` meanwhile.
    void call_guest(ExecState& state, R5900Context& ctx);
    // IGuestCalls (overrides.h): native override code calling guest functions and kernel services on the
    // current thread's fiber.
    std::uint64_t call_function(R5900Context& ctx, std::uint32_t entry, std::span<const std::uint64_t> args,
                                std::uint32_t sp) override;
    std::uint64_t call_kernel(R5900Context& ctx, std::string_view name,
                              std::span<const std::uint64_t> args) override;
    // Fills a stop report's last branch.
    void annotate_stop(StopInfo& info) const;
    // Ends the run with `info` (MachineServices::stop: a fiber switch on a fiber, a throw elsewhere).
    [[noreturn]] void stop(StopInfo info);

    // ---- The PS2Runtime surface, one member per dq8_runtime_api entry.
    bool dispatch_guest_branch(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t target_pc,
                               std::uint32_t source_pc, std::uint32_t fallthrough_pc, std::uint32_t kind,
                               const char* debug_name);
    void checkpoint(std::uint32_t cycles);
    void handle_syscall(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t encoded_id);
    void call_named(std::uint32_t kind, const char* name, std::uint8_t* rdram, R5900Context* ctx);
    [[noreturn]] void signal_exception(R5900Context* ctx, std::uint32_t exception);
    [[noreturn]] void stop_unsupported(R5900Context* ctx, std::string_view what, std::uint32_t address = 0);

    // The table every adapter points at. Its functions take an EeRuntime* as `host`.
    static const dq8_runtime_api& api() noexcept;

  private:
    ExecState& state_for(R5900Context* ctx);
    void run_from_pc(ExecState& state, R5900Context& ctx, std::uint32_t base_depth);
    void count_dispatch(std::uint32_t target, std::uint32_t source, std::string_view how,
                        std::string_view unit);
    [[noreturn]] void stop_missing(std::uint32_t pc, std::uint32_t target, std::uint32_t source,
                                   std::string_view how);
    void run_syscall_handler(ExecState& state, R5900Context& ctx, std::int32_t number, std::uint32_t handler);
    void trace_kernel_call(std::string_view name, const R5900Context& ctx);

    GuestMemory& memory_;
    const Dispatcher& dispatcher_;
    MachineServices& machine_;
    KernelServices& services_;
    EeKernel* kernel_ = nullptr;
    ExecState* exec_ = nullptr;
    std::unique_ptr<ExecState> offline_; // for calls made off any fiber (host-side tests)
    std::unique_ptr<R5900Context> power_on_;
    PS2Runtime adapter_;
    TraceSink trace_;
    RunOptions options_;
    std::uint64_t dispatches_ = 0;
    std::string last_branch_;
    std::uint32_t last_source_ = 0;
    std::uint32_t last_target_ = 0;
};

// The display name of a dq8_branch_kind ("DirectCall", ...).
std::string_view branch_kind_name(std::uint32_t kind) noexcept;
// The display name of a dq8_call_kind ("syscall", "stub", "todo").
std::string_view call_kind_name(std::uint32_t kind) noexcept;

} // namespace dq8::rt
