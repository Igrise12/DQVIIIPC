// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/overrides.h"

#include "runtime/ee/context.h"

#include <stdexcept>
#include <vector>

namespace dq8::rt {

std::string_view to_string(OverrideKind kind) noexcept {
    switch (kind) {
    case OverrideKind::replace:
        return "replace";
    case OverrideKind::hook_before:
        return "hook_before";
    case OverrideKind::hook_after:
        return "hook_after";
    }
    return "unknown";
}

void OverrideCall::return_to_caller() const noexcept {
    ctx->pc = gpr_u32(*ctx, 31);
}

namespace {

constexpr unsigned reg_sp = 29;

std::vector<std::uint64_t> argument_list(std::initializer_list<std::uint64_t> args) {
    if (args.size() > 8) {
        throw std::invalid_argument("OverrideCall: at most eight arguments ($a0..$a3, $t0..$t3)");
    }
    return std::vector<std::uint64_t>(args);
}

} // namespace

std::uint32_t OverrideCall::scratch(std::uint32_t bytes) {
    const std::uint32_t top = scratch_sp != 0 ? scratch_sp : gpr_u32(*ctx, reg_sp);
    scratch_sp = (top - bytes) & ~0xFu;
    return scratch_sp;
}

std::uint64_t OverrideCall::call_guest(std::uint32_t entry, std::initializer_list<std::uint64_t> args) {
    if (guest == nullptr) {
        throw std::logic_error("OverrideCall::call_guest: no IGuestCalls (Dispatcher::set_guest_calls)");
    }
    const std::uint32_t top = scratch_sp != 0 ? scratch_sp : gpr_u32(*ctx, reg_sp);
    const std::vector<std::uint64_t> list = argument_list(args);
    return guest->call_function(*ctx, entry, list, (top - 16u) & ~0xFu);
}

std::uint64_t OverrideCall::kernel_call(std::string_view name, std::initializer_list<std::uint64_t> args) {
    if (guest == nullptr) {
        throw std::logic_error("OverrideCall::kernel_call: no IGuestCalls (Dispatcher::set_guest_calls)");
    }
    const std::vector<std::uint64_t> list = argument_list(args);
    return guest->call_kernel(*ctx, name, list);
}

} // namespace dq8::rt
