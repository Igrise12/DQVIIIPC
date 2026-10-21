// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// EE kernel call numbers and names (the public SDK numbering; interrupt-context variants use the negated
// number). The numbers and the handler each one reaches follow PS2Recomp eb8688b's numeric syscall dispatcher
// (ps2xRuntime/src/lib/Kernel/Syscalls/Dispatcher.cpp), so a syscall instruction and a by-name call from
// generated code (ps2_syscalls::Name) meet on the same name. Names are the handler names PS2Recomp emits; a
// few kernel calls have no by-name form in PS2Recomp's call list and are reachable by number only. M3 added
// the numbers missing from PS2Recomp's dispatcher (0x06, 0x0D, 0x6B, 0x7B, 0x80) under their SDK names.
namespace dq8::rt {

struct KernelCall {
    std::int32_t number = 0;
    std::string_view name;
    // Another number is the canonical one for this name; kernel_call_number() skips aliases.
    bool alias = false;
};

// Every known kernel call, each number once.
std::span<const KernelCall> kernel_calls() noexcept;

// The name for a number as the guest passes it in $v1 or the syscall code (a negative number arrives as its
// two's complement), or nullopt.
std::optional<std::string_view> kernel_call_name(std::uint32_t number) noexcept;

// The canonical number for a name, or nullopt.
std::optional<std::int32_t> kernel_call_number(std::string_view name) noexcept;

// "0x3C", or "-0x1A" for an interrupt-context call.
std::string format_kernel_call_number(std::uint32_t number);

} // namespace dq8::rt
