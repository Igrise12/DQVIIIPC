// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/overrides.h"

#include "runtime/ee/context.h"

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

} // namespace dq8::rt
