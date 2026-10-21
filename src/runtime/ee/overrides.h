// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/abi/game_module.h"
#include "dq8/abi/ps2recomp/r5900_context.h"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

class PS2Runtime;

// Overrides (architecture §4.5): native code that replaces or wraps a guest function, keyed by (region
// profile, code unit, guest address); the game layer (src/game) declares them from profile data and the
// runtime never names an address. The dispatcher keeps its own copies of the dispatch tables and points an
// overridden slot at one trampoline, which finds the override by ctx->pc, so resolve() costs the same; only
// dispatched entries (jal, jalr, jr, run-loop entries) are intercepted, not a same-unit `j` tail call or a
// branch inside the function. Installation is Dispatcher::install_override (dispatcher.h).
namespace dq8::rt {

class MachineServices;

enum class OverrideKind {
    replace,     // the native function runs instead of the guest one
    hook_before, // runs, then the guest function from its entry
    hook_after,  // the guest function runs, then this
};

std::string_view to_string(OverrideKind kind) noexcept;

// Calls from native override code into the guest (M4 day 0; the sceMpeg HLE calls the player's callbacks).
// Implemented by the EE runtime; the dispatcher hands it to every override (Dispatcher::set_guest_calls).
class IGuestCalls {
  public:
    virtual ~IGuestCalls() = default;
    // Runs the guest function at `entry` as a nested call on a copy of `ctx`'s registers: $a0..$a3 and
    // $t0..$t3 from `args`, $sp = `sp`, $ra = 0. The callee may block, yield or be terminated with its thread
    // (the fiber is then abandoned with every host frame on it, so callers keep no state in locals that must
    // be destroyed). Returns the callee's $v0 (low 64 bits); `ctx` is unchanged.
    virtual std::uint64_t call_function(R5900Context& ctx, std::uint32_t entry,
                                        std::span<const std::uint64_t> args, std::uint32_t sp) = 0;
    // A kernel service by name, as the guest's by-name call would make it (it may block); returns $v0.
    virtual std::uint64_t call_kernel(R5900Context& ctx, std::string_view name,
                                      std::span<const std::uint64_t> args) = 0;
};

struct OverrideCall {
    std::uint8_t* rdram = nullptr;
    R5900Context* ctx = nullptr;
    PS2Runtime* runtime = nullptr;
    MachineServices* machine = nullptr; // Dispatcher::set_machine; null in tests that run without one
    dq8_guest_fn original = nullptr;    // the recompiled function, for wrappers that call it themselves
    std::uint32_t address = 0;          // the overridden entry (ctx->pc when the trampoline was entered)
    IGuestCalls* guest = nullptr;       // Dispatcher::set_guest_calls; null without a machine
    // A replacement returns like a guest function: it sets ctx->pc to $ra (the trampoline checks in debug).
    void return_to_caller() const noexcept;

    // Guest memory below the caller's stack pointer for data a callee reads (a callback block): 16-byte
    // aligned, valid until the override returns. Each call returns a new block below the previous one.
    std::uint32_t scratch(std::uint32_t bytes);
    // Calls the guest function at `entry` with up to eight arguments; its stack starts below every scratch
    // block. Returns its $v0. std::logic_error without IGuestCalls.
    std::uint64_t call_guest(std::uint32_t entry, std::initializer_list<std::uint64_t> args = {});
    // Calls a kernel service by name ("RotateThreadReadyQueue") with up to eight arguments; returns $v0.
    std::uint64_t kernel_call(std::string_view name, std::initializer_list<std::uint64_t> args = {});

    // Internal to scratch(): 0 until the first block, then the lowest block's address. Public so that
    // OverrideCall stays an aggregate.
    std::uint32_t scratch_sp = 0;
};

using OverrideFn = std::function<void(OverrideCall&)>;

struct OverrideSpec {
    std::string unit;          // "main" or an overlay unit name
    std::uint32_t address = 0; // a function entry of that unit
    OverrideKind kind = OverrideKind::replace;
    std::string name; // for traces and reports: "overlay-residency-hook"
    OverrideFn fn;
    // Optional digest of the guest bytes the override assumes ([address, address + code_bytes) in guest RAM),
    // as 40 hex digits; a mismatch refuses the override. Main-unit digests are checked at installation;
    // overlay-unit digests on the first call after each residency change (the bytes are not in RAM before).
    std::uint32_t code_bytes = 0;
    std::string code_sha1;
};

// One installed override, for reports and --json.
struct OverrideReport {
    std::string unit;
    std::uint32_t address = 0;
    OverrideKind kind = OverrideKind::replace;
    std::string name;
    std::uint64_t calls = 0;
};

} // namespace dq8::rt
