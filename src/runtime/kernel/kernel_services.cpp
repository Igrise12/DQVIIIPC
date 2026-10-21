// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/kernel/kernel_services.h"

#include "dq8/abi/game_module.h"
#include "runtime/ee/context.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/kernel/kernel_calls.h"
#include "runtime/machine/machine_services.h"

#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace dq8::rt {

GuestMemory& KernelCallContext::mem() const {
    return machine_.memory();
}

std::uint32_t KernelCallContext::arg(unsigned i) const {
    if (i > 7) {
        throw std::out_of_range("kernel call argument index " + std::to_string(i));
    }
    return gpr_u32(ctx_, i < 4 ? 4 + i : 8 + (i - 4));
}

std::uint64_t KernelCallContext::arg64(unsigned i) const {
    if (i > 7) {
        throw std::out_of_range("kernel call argument index " + std::to_string(i));
    }
    return gpr_lo(ctx_, i < 4 ? 4 + i : 8 + (i - 4));
}

void KernelCallContext::ret(std::int32_t value) noexcept {
    set_gpr_lo(ctx_, 2, static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
}

void KernelCallContext::ret64(std::uint64_t value) noexcept {
    set_gpr_lo(ctx_, 2, value);
}

void KernelCallContext::unimplemented(std::string_view why) const {
    StopInfo info;
    info.reason = has_number_ ? StopReason::syscall : StopReason::named_call;
    info.pc = ctx_.pc;
    info.has_number = has_number_;
    info.number = static_cast<std::uint32_t>(number_);
    info.call_name = std::string(called_as_);
    info.call_kind = has_number_ ? 0u : static_cast<std::uint32_t>(DQ8_CALL_SYSCALL);
    for (unsigned i = 0; i < 4; ++i) {
        info.args[i] = arg(i);
    }
    info.message =
        "kernel call " + std::string(called_as_) +
        (has_number_ ? " (" + format_kernel_call_number(static_cast<std::uint32_t>(number_)) + ")" : "") +
        " is not implemented: " + std::string(why);
    machine_.stop(std::move(info));
    std::abort(); // MachineServices::stop never returns; GCC does not trust [[noreturn]] through a virtual
                  // call
}

KernelServices::KernelServices() {
    alias("SetGsCrt", "GsSetCrt");
    alias("isceSifSetDma", "sceSifSetDma", true);
    alias("isceSifSetDChain", "sceSifSetDChain", true);
}

void KernelServices::add(KernelServiceInfo info) {
    if (info.name.empty()) {
        throw std::invalid_argument("kernel service with an empty name");
    }
    const bool noop = (info.flags & kernel_service_noop) != 0;
    if (!noop && !info.fn) {
        throw std::invalid_argument("kernel service " + info.name + " has no function");
    }
    if (noop && info.note.empty()) {
        throw std::invalid_argument("kernel service " + info.name + " is a noop without a note saying why");
    }
    if (aliases_.count(info.name) != 0) {
        throw std::invalid_argument("kernel service " + info.name + " is registered as an alias");
    }
    std::string key = info.name;
    if (services_.count(key) != 0) {
        throw std::invalid_argument("kernel service " + key + " is registered twice");
    }
    services_.emplace(std::move(key), std::move(info));
}

void KernelServices::alias(std::string alias_name, std::string canonical, bool interrupt_variant) {
    if (alias_name.empty() || canonical.empty() || alias_name == canonical) {
        throw std::invalid_argument("kernel service alias '" + alias_name + "' -> '" + canonical + "'");
    }
    if (services_.count(alias_name) != 0) {
        throw std::invalid_argument("kernel service alias " + alias_name + " is already a service name");
    }
    aliases_[std::move(alias_name)] = Alias{std::move(canonical), interrupt_variant};
}

KernelResolution KernelServices::resolve(std::string_view name, bool has_number, std::int32_t number,
                                         bool interrupt) const {
    KernelResolution r;
    r.name = name;
    r.has_number = has_number;
    r.number = number;
    r.interrupt_context = interrupt;
    if (has_number) {
        if (const auto it = guest_handlers_.find(number); it != guest_handlers_.end()) {
            r.kind = KernelResolution::Kind::guest_handler;
            r.guest_address = it->second;
            return r;
        }
    }
    if (const auto it = services_.find(name); it != services_.end()) {
        r.kind = KernelResolution::Kind::native;
        r.service = &it->second;
        r.name = it->first;
    }
    return r;
}

KernelResolution KernelServices::resolve_number(std::uint32_t number) const {
    const auto n = static_cast<std::int32_t>(number);
    const auto name = kernel_call_name(number);
    if (!name) {
        // An unknown number can still have a guest handler.
        KernelResolution r = resolve({}, true, n, n < 0);
        return r;
    }
    return resolve(*name, true, n, n < 0);
}

KernelResolution KernelServices::resolve_name(std::string_view name) const {
    std::string_view canonical = name;
    bool interrupt = false;
    if (const auto it = aliases_.find(name); it != aliases_.end()) {
        canonical = it->second.canonical;
        interrupt = it->second.interrupt_variant;
    }
    const auto number = kernel_call_number(canonical);
    if (number && *number < 0) {
        interrupt = true;
    }
    KernelResolution r = resolve(canonical, number.has_value(), number.value_or(0), interrupt);
    if (r.kind == KernelResolution::Kind::missing) {
        r.name = name; // report the spelling the guest used
    }
    return r;
}

void KernelServices::install_guest_handler(std::int32_t number, std::uint32_t address) {
    guest_handlers_[number] = address;
}

void KernelServices::remove_guest_handler(std::int32_t number) {
    guest_handlers_.erase(number);
}

std::optional<std::uint32_t> KernelServices::guest_handler(std::int32_t number) const {
    const auto it = guest_handlers_.find(number);
    if (it == guest_handlers_.end()) {
        return std::nullopt;
    }
    return it->second;
}

const KernelServiceInfo* KernelServices::find(std::string_view name) const {
    const auto it = services_.find(name);
    return it == services_.end() ? nullptr : &it->second;
}

std::vector<const KernelServiceInfo*> KernelServices::services() const {
    std::vector<const KernelServiceInfo*> out;
    out.reserve(services_.size());
    for (const auto& [name, info] : services_) {
        out.push_back(&info);
    }
    return out;
}

void KernelServices::invoke(const KernelServiceInfo& service, KernelCallContext& call) {
    auto it = counts_.find(service.name);
    if (it == counts_.end()) {
        it = counts_.emplace(service.name, 0).first;
    }
    ++it->second;
    if ((service.flags & kernel_service_noop) != 0) {
        if (strict_) {
            call.unimplemented("noop in strict mode (" + service.note + ")");
        }
        if (!service.fn) {
            return;
        }
    }
    service.fn(call);
}

std::uint64_t KernelServices::call_count(std::string_view name) const {
    const auto it = counts_.find(name);
    return it == counts_.end() ? 0 : it->second;
}

} // namespace dq8::rt
