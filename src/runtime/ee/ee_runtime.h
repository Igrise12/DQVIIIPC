// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/abi/game_module.h"
#include "dq8/abi/ps2recomp/ps2_runtime.h"
#include "runtime/ee/dispatcher.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/kernel/kernel_hle.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

// The EE side of the host runtime: the dq8_runtime_api table that generated code reaches through its
// PS2Runtime adapter, the dispatch contract for every jal/jalr/jr, and the run loop. One guest thread, no
// scheduler, no virtual clock yet (M2): run() mirrors PS2Recomp's EeScheduler::run for a single thread and
// ends with a StopInfo instead of a thread switch. Behaviour follows PS2Runtime::dispatchGuestBranch at
// eb8688b; the differences are listed in src/runtime/README.md.
namespace dq8::rt {

struct RunOptions {
    bool stop_at_first_syscall = false; // stop at a kernel call even when the kernel HLE implements it
    std::uint64_t max_dispatches = 0;   // 0: no limit. Counts run-loop entries and every dispatched branch
    std::uint32_t max_call_depth =
        4096;                    // nested guest calls on the host stack before the guard stops the guest
    bool trace_dispatch = false; // one trace line per dispatch
};

struct RunResult {
    StopInfo stop;
    std::uint64_t dispatches = 0;
};

class EeRuntime {
  public:
    using TraceSink = std::function<void(std::string_view line)>;

    // The references must outlive the runtime. The context starts in its power-on state with pc 0; call
    // reset_context() (context.h) or set pc before run().
    EeRuntime(GuestMemory& memory, const Dispatcher& dispatcher, const KernelHle& kernel);
    EeRuntime(const EeRuntime&) = delete;
    EeRuntime& operator=(const EeRuntime&) = delete;

    R5900Context& context() noexcept { return ctx_; }
    const R5900Context& context() const noexcept { return ctx_; }
    GuestMemory& memory() noexcept { return memory_; }
    // What generated code receives as `runtime`.
    PS2Runtime* adapter() noexcept { return &adapter_; }
    // Trace lines go here (default: stderr).
    void set_trace_sink(TraceSink sink) { trace_ = std::move(sink); }

    // Runs from context().pc until the guest stops. Never throws for guest behaviour: every stop, including a
    // C++ exception from generated code, ends up in the result.
    RunResult run(const RunOptions& options = {});

    // The PS2Runtime surface, one member per dq8_runtime_api entry. They act on context() and memory();
    // the rdram/ctx arguments the generated code passes are those objects.
    bool dispatch_guest_branch(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t target_pc,
                               std::uint32_t source_pc, std::uint32_t fallthrough_pc, std::uint32_t kind,
                               const char* debug_name);
    void handle_syscall(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t encoded_id);
    void call_named(std::uint32_t kind, const char* name, std::uint8_t* rdram, R5900Context* ctx);
    [[noreturn]] void signal_exception(R5900Context* ctx, std::uint32_t exception);
    [[noreturn]] void stop_unsupported(R5900Context* ctx, std::string_view what, std::uint32_t address = 0);

    // The table every adapter points at. Its functions take an EeRuntime* as `host`.
    static const dq8_runtime_api& api() noexcept;

  private:
    void count_dispatch(std::uint32_t target, std::uint32_t source, std::string_view how,
                        std::string_view unit);
    [[noreturn]] void stop_missing(std::uint32_t target, std::uint32_t source, std::string_view how);

    GuestMemory& memory_;
    const Dispatcher& dispatcher_;
    const KernelHle& kernel_;
    R5900Context ctx_;
    PS2Runtime adapter_;
    TraceSink trace_;
    RunOptions options_;
    std::uint64_t dispatches_ = 0;
    std::uint32_t depth_ = 0;
    std::string last_branch_;
    std::uint32_t last_source_ = 0;
    std::uint32_t last_target_ = 0;
};

// The display name of a dq8_branch_kind ("DirectCall", ...).
std::string_view branch_kind_name(std::uint32_t kind) noexcept;
// The display name of a dq8_call_kind ("syscall", "stub", "todo").
std::string_view call_kind_name(std::uint32_t kind) noexcept;

} // namespace dq8::rt
