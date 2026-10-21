// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/abi/game_module.h"
#include "dq8/abi/ps2recomp/ps2_runtime.h"
#include "runtime/ee/context.h"
#include "runtime/ee/dispatch_table.h"
#include "runtime/ee/dispatcher.h"
#include "runtime/ee/ee_runtime.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/ee/overlay_manager.h"
#include "runtime/kernel/ee_kernel.h"
#include "runtime/machine/interrupts.h"
#include "runtime/machine/machine.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The execution core's test harness (session A): guest programs made of host functions in the emitter's
// calling convention, run on a real Machine (fibers, scheduler, kernel services) with no devices. Synthetic
// only. Device-level tests use fake_machine.h instead.
namespace dq8::test {

// A guest program made of host functions: one dispatch table over [base, end).
class HostProgram {
  public:
    static constexpr std::uint32_t base = 0x00300000u;
    static constexpr std::uint32_t end = 0x00310000u;

    HostProgram() : slots_((end - base) / 4, nullptr) {}
    void set(std::uint32_t address, dq8_guest_fn fn) { slots_.at((address - base) / 4) = fn; }
    rt::DispatchTable table() const { return rt::DispatchTable(base, end, slots_); }

  private:
    std::vector<dq8_guest_fn> slots_;
};

// A Machine over one dispatch table, with every emitted event kept.
struct TestMachine {
    rt::GuestMemory memory;
    rt::OverlayManager overlays;
    rt::Dispatcher dispatcher{overlays};
    std::unique_ptr<rt::Machine> machine;
    std::vector<rt::MachineEvent> events;

    explicit TestMachine(const rt::DispatchTable& table, rt::MachineConfig config = {},
                         std::string unit = "test") {
        dispatcher.set_main(std::move(unit), table);
        machine = std::make_unique<rt::Machine>(memory, dispatcher, config);
        machine->set_event_sink([this](const rt::MachineEvent& e) { events.push_back(e); });
    }

    void boot(std::uint32_t entry) { machine->boot(entry); }
    rt::SliceResult run(rt::RunLimits limits = {}) { return machine->run(limits); }
    rt::SliceResult run_from(std::uint32_t entry, rt::RunLimits limits = {}) {
        boot(entry);
        return run(limits);
    }

    rt::EeKernel& kernel() { return dynamic_cast<rt::EeKernel&>(*machine->scheduler()); }
    R5900Context& ctx() { return machine->ee().context(); }
    std::uint32_t reg(unsigned r) { return rt::gpr_u32(ctx(), r); }
    std::uint32_t word(std::uint32_t address) { return memory.load32(address); }
    void set_word(std::uint32_t address, std::uint32_t value) { memory.store32(address, value); }

    // A kernel call made from the host, off any fiber (no thread is current): for calls that never block.
    // Returns $v0.
    std::int32_t call(std::int32_t number, std::initializer_list<std::uint32_t> args = {}) {
        R5900Context c;
        unsigned reg_index = 4; // $a0..$a3, then $t0..$t3 (registers 8..11): one contiguous run
        for (const std::uint32_t a : args) {
            rt::set_gpr_lo(
                c, reg_index++,
                static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(a))));
        }
        c.pc = 0x00300004u;
        machine->ee().handle_syscall(memory.rdram(), &c, static_cast<std::uint32_t>(number));
        return static_cast<std::int32_t>(rt::gpr_u32(c, 2));
    }
    // The same for a by-name call (ps2_syscalls::Name).
    std::int32_t named(const char* name, std::initializer_list<std::uint32_t> args = {}) {
        R5900Context c;
        unsigned reg_index = 4;
        for (const std::uint32_t a : args) {
            rt::set_gpr_lo(
                c, reg_index++,
                static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(a))));
        }
        c.pc = 0x00300004u;
        machine->ee().call_named(DQ8_CALL_SYSCALL, name, memory.rdram(), &c);
        return static_cast<std::int32_t>(rt::gpr_u32(c, 2));
    }
};

// Stands in for session B's INTC vblank phase: raises INTC 2 at vblank start and INTC 3 at vblank end, from
// field `first` on.
class VblankIrqDevice final : public rt::IDevice, public rt::IVblankListener {
  public:
    explicit VblankIrqDevice(std::uint64_t first = 1) : first_(first) {}
    std::string_view name() const override { return "test-vblank-irq"; }
    void attach(rt::MachineServices& machine) override {
        machine_ = &machine;
        machine.add_vblank_listener(rt::VblankPhase::intc, *this);
    }
    void reset() override {}
    void on_vblank_start(std::uint64_t field) override {
        if (field >= first_) {
            machine_->interrupts().raise_intc(rt::IntcLine::vblank_start);
        }
    }
    void on_vblank_end(std::uint64_t field) override {
        if (field >= first_) {
            machine_->interrupts().raise_intc(rt::IntcLine::vblank_end);
        }
    }

  private:
    std::uint64_t first_;
    rt::MachineServices* machine_ = nullptr;
};

// ---- Helpers for host-function guest code (called with the generated code's arguments).

inline void set_arg(R5900Context* ctx, unsigned i, std::uint32_t value) {
    rt::set_gpr_lo(*ctx, i < 4 ? 4 + i : 8 + (i - 4),
                   static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value))));
}

// A syscall as generated code makes it: $v1 = number, pc = the next instruction, then the host call.
inline std::int32_t guest_syscall(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime,
                                  std::int32_t number, std::initializer_list<std::uint32_t> args = {}) {
    unsigned i = 0;
    for (const std::uint32_t a : args) {
        set_arg(ctx, i++, a);
    }
    rt::set_gpr_lo(*ctx, 3, static_cast<std::uint64_t>(static_cast<std::int64_t>(number)));
    runtime->handleSyscall(rdram, ctx, 0);
    return static_cast<std::int32_t>(rt::gpr_u32(*ctx, 2));
}

// A by-name kernel call (ps2_syscalls::Name) as generated stubs make it.
inline std::int32_t guest_named(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime, const char* name,
                                std::initializer_list<std::uint32_t> args = {}) {
    unsigned i = 0;
    for (const std::uint32_t a : args) {
        set_arg(ctx, i++, a);
    }
    runtime->api->call_named(runtime->host, DQ8_CALL_SYSCALL, name, rdram, ctx);
    return static_cast<std::int32_t>(rt::gpr_u32(*ctx, 2));
}

// `jr $ra`.
inline void guest_return(R5900Context* ctx) {
    ctx->pc = rt::gpr_u32(*ctx, 31);
}

// Guest RAM words through the host pointer generated code receives (physical addresses below 32 MiB).
inline void poke(std::uint8_t* rdram, std::uint32_t address, std::uint32_t value) {
    std::memcpy(rdram + (address & 0x01FFFFFFu), &value, sizeof value);
}
inline std::uint32_t peek(const std::uint8_t* rdram, std::uint32_t address) {
    std::uint32_t value = 0;
    std::memcpy(&value, rdram + (address & 0x01FFFFFFu), sizeof value);
    return value;
}

// CreateThread's parameter block at `param` (kernel_abi.h layout), then the call. Returns the thread id.
inline std::int32_t guest_create_thread(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime,
                                        std::uint32_t param, std::uint32_t entry, std::uint32_t stack,
                                        std::uint32_t stack_size, std::int32_t priority) {
    poke(rdram, param + 0x04, entry);
    poke(rdram, param + 0x08, stack);
    poke(rdram, param + 0x0C, stack_size);
    poke(rdram, param + 0x10, 0);
    poke(rdram, param + 0x14, static_cast<std::uint32_t>(priority));
    return guest_syscall(rdram, ctx, runtime, 0x20, {param});
}

// CreateSema's parameter block at `param`, then the call. Returns the semaphore id.
inline std::int32_t guest_create_sema(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime,
                                      std::uint32_t param, std::int32_t init_count, std::int32_t max_count) {
    poke(rdram, param + 0x04, static_cast<std::uint32_t>(max_count));
    poke(rdram, param + 0x08, static_cast<std::uint32_t>(init_count));
    poke(rdram, param + 0x10, 0);
    poke(rdram, param + 0x14, 0);
    return guest_syscall(rdram, ctx, runtime, 0x40, {param});
}

// Ends the run from guest code with StopReason::requested.
[[noreturn]] inline void guest_request_stop(rt::Machine& machine, std::string message = "test done") {
    rt::StopInfo s;
    s.reason = rt::StopReason::requested;
    s.message = std::move(message);
    machine.stop(std::move(s));
}

} // namespace dq8::test
