// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <array>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

// GuestStop ends a run: a host hook throws it and EeRuntime::run() catches it, unwinding through the
// generated code's frames on the way (Docs/milestones/M2-first-recompile.md, "M2 deviation from architecture
// §13.3"). M3 replaces the throw with a fiber switch; StopInfo stays the report.
namespace dq8::rt {

// Why the guest stopped. The spellings from to_string() are printed in dq8-game's --json output; add values,
// never rename them.
enum class StopReason {
    syscall,          // a `syscall` the kernel HLE does not handle (any one, with stop-at-first-syscall)
    named_call,       // a by-name kernel call, stub or TODO from generated code with no host implementation
    program_end,      // pc reached 0: the entry function returned
    missing_function, // a dispatch to an address with no recompiled function
    max_dispatches,   // the dispatch budget ran out
    call_depth,       // nested guest calls exceeded the host-stack guard
    mmio_trap,        // an access to a device range while trap-mmio is set
    guest_exception,  // trap, break or an exception the generated code signalled
    unsupported,      // a service M2 does not provide (TLB instructions, VU0 microprograms, GIF DMA kicks)
    guest_error,      // the generated code threw, e.g. on an instruction the recompiler did not translate
};

std::string_view to_string(StopReason reason) noexcept;

struct StopInfo {
    StopReason reason = StopReason::program_end;
    std::string message;       // one line for people
    std::uint32_t pc = 0;      // ctx->pc when the stop was raised (for a syscall: the instruction after it)
    std::uint32_t address = 0; // the address the stop is about: the syscall instruction, the missing target,
                               // the device register, ...
    std::uint32_t source = 0;  // the branch that led here, when known (missing_function)
    // syscall and named_call: the kernel call number as the guest passed it (a negative number as its two's
    // complement); for guest_exception, the exception code.
    bool has_number = false;
    std::uint32_t number = 0;
    std::string call_name;               // kernel call, stub or TODO name, when known
    std::uint32_t call_kind = 0;         // dq8_call_kind for named_call, 0 otherwise
    std::array<std::uint32_t, 4> args{}; // $a0..$a3 for syscall and named_call
    // The last dispatched branch before the stop, filled in by EeRuntime::run() (empty when there was none).
    std::string last_branch; // the emitter's debug name ("JAL", "JALR", "JR", ...) or "run-loop"
    std::uint32_t last_source = 0;
    std::uint32_t last_target = 0;
};

class GuestStop : public std::exception {
  public:
    explicit GuestStop(StopInfo info) : info_(std::move(info)) {}
    const StopInfo& info() const noexcept { return info_; }
    const char* what() const noexcept override { return info_.message.c_str(); }

  private:
    StopInfo info_;
};

// "0x0010001C": the form every runtime message uses for guest addresses.
std::string hex32(std::uint32_t value);

} // namespace dq8::rt
