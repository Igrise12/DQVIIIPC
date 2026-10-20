// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/abi/ps2recomp/r5900_context.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

// Kernel and library services implemented natively (architecture §5.1), keyed by name: a syscall instruction
// is mapped to its name through kernel_calls.h, a by-name call from generated code arrives with its name. M2
// registers nothing, so every call stops the guest; M3 fills this in.
namespace dq8::rt {

class KernelHle {
  public:
    using Handler = std::function<void(std::uint8_t* rdram, R5900Context* ctx)>;

    // Replaces any handler with the same name.
    void register_handler(std::string name, Handler handler);
    bool has_handler(std::string_view name) const;

    // Runs the handler for a kernel call number and returns true, or returns false when there is none.
    bool handle_syscall(std::uint32_t number, std::uint8_t* rdram, R5900Context* ctx) const;
    // Runs the handler for a by-name call (any dq8_call_kind) and returns true, or returns false.
    bool handle_named(std::string_view name, std::uint8_t* rdram, R5900Context* ctx) const;

  private:
    std::map<std::string, Handler, std::less<>> handlers_;
};

} // namespace dq8::rt
