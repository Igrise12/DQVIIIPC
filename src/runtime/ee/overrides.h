// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/abi/game_module.h"
#include "dq8/abi/ps2recomp/r5900_context.h"

#include <cstdint>
#include <functional>
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

struct OverrideCall {
    std::uint8_t* rdram = nullptr;
    R5900Context* ctx = nullptr;
    PS2Runtime* runtime = nullptr;
    MachineServices* machine = nullptr; // Dispatcher::set_machine; null in tests that run without one
    dq8_guest_fn original = nullptr;    // the recompiled function, for wrappers that call it themselves
    std::uint32_t address = 0;          // the overridden entry (ctx->pc when the trampoline was entered)
    // A replacement returns like a guest function: it sets ctx->pc to $ra (the trampoline checks in debug).
    void return_to_caller() const noexcept;
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
