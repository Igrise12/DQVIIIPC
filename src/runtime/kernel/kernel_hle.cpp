// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/kernel/kernel_hle.h"

#include "runtime/kernel/kernel_calls.h"

#include <utility>

namespace dq8::rt {

void KernelHle::register_handler(std::string name, Handler handler) {
    handlers_[std::move(name)] = std::move(handler);
}

bool KernelHle::has_handler(std::string_view name) const {
    return handlers_.find(name) != handlers_.end();
}

bool KernelHle::handle_syscall(std::uint32_t number, std::uint8_t* rdram, R5900Context* ctx) const {
    const auto name = kernel_call_name(number);
    return name && handle_named(*name, rdram, ctx);
}

bool KernelHle::handle_named(std::string_view name, std::uint8_t* rdram, R5900Context* ctx) const {
    const auto it = handlers_.find(name);
    if (it == handlers_.end() || !it->second) {
        return false;
    }
    it->second(rdram, ctx);
    return true;
}

} // namespace dq8::rt
