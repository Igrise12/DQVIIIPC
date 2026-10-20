// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_stop.h"

#include <cstdio>

namespace dq8::rt {

std::string_view to_string(StopReason reason) noexcept {
    switch (reason) {
    case StopReason::syscall:
        return "syscall";
    case StopReason::named_call:
        return "named-call";
    case StopReason::program_end:
        return "program-end";
    case StopReason::missing_function:
        return "missing-function";
    case StopReason::max_dispatches:
        return "max-dispatches";
    case StopReason::call_depth:
        return "call-depth";
    case StopReason::mmio_trap:
        return "mmio-trap";
    case StopReason::guest_exception:
        return "guest-exception";
    case StopReason::unsupported:
        return "unsupported";
    case StopReason::guest_error:
        return "guest-error";
    }
    return "unknown";
}

std::string hex32(std::uint32_t value) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", static_cast<unsigned>(value));
    return buf;
}

} // namespace dq8::rt
