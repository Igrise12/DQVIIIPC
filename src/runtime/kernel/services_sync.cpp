// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xRuntime/src/lib/Kernel/Syscalls/Sync.cpp (semaphores, event flags, alarms), re-signed to
// KernelCallContext. What changed: alarms count hblanks on the virtual clock; status blocks at address 0 are
// refused.
#include "runtime/ee/context.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/kernel/ee_kernel.h"
#include "runtime/kernel/ee_services.h"
#include "runtime/kernel/kernel_abi.h"
#include "runtime/kernel/kernel_services.h"

#include <utility>

namespace dq8::rt {

namespace {

using detail::guest_block_ok;
using detail::read_u32;
using detail::write_u32;

// The checks WaitEventFlag and PollEventFlag share; returns 0 when the call may go ahead.
std::int32_t check_event_wait(EeKernel& kernel, std::int32_t id, std::uint32_t bits, std::uint32_t mode,
                              std::uint32_t result_address) {
    if ((mode & ~kabi::event_mode_mask) != 0) {
        return kabi::illegal_mode;
    }
    if (bits == 0) {
        return kabi::event_illegal_pattern;
    }
    const EventFlag* flag = kernel.event_flag(id);
    if (flag == nullptr) {
        return kabi::unknown_event_flag_id;
    }
    if ((flag->attr & kabi::event_attr_multi) == 0 && !flag->waiters.empty()) {
        return kabi::event_multi;
    }
    if (result_address != 0 && !guest_block_ok(result_address, 4)) {
        return kabi::error;
    }
    return kabi::ok;
}

} // namespace

void register_sync_services(KernelServices& services, EeKernel& kernel) {
    const auto add = [&services](std::string name, KernelServiceFn fn, std::uint32_t flags = 0) {
        services.add(KernelServiceInfo{std::move(name), std::move(fn), flags, {}, "kernel"});
    };
    constexpr std::uint32_t blocking = kernel_service_blocking;
    constexpr std::uint32_t irq_ok = kernel_service_interrupt_ok;

    // ---- Semaphores.
    add("CreateSema", [&kernel](KernelCallContext& c) {
        const std::uint32_t param = c.arg(0);
        if (param == 0 || !guest_block_ok(param, kabi::sema_param::size)) {
            c.ret(kabi::error);
            return;
        }
        GuestMemory& mem = c.mem();
        namespace sp = kabi::sema_param;
        // The EE layout: the initial count is at 0x08; the count field at 0x00 is status-only.
        c.ret(kernel.create_semaphore(static_cast<std::int32_t>(read_u32(mem, param + sp::init_count)),
                                      static_cast<std::int32_t>(read_u32(mem, param + sp::max_count)),
                                      read_u32(mem, param + sp::attr), read_u32(mem, param + sp::option)));
    });
    const auto delete_sema = [&kernel](KernelCallContext& c) { c.ret(kernel.delete_semaphore(c.sarg(0))); };
    add("DeleteSema", delete_sema);
    add("iDeleteSema", delete_sema, irq_ok);
    const auto signal = [&kernel](KernelCallContext& c) { c.ret(kernel.signal_semaphore(c.sarg(0))); };
    add("SignalSema", signal);
    add("iSignalSema", signal, irq_ok);
    add("WaitSema", [&kernel](KernelCallContext& c) { c.ret(kernel.wait_semaphore(c.sarg(0))); }, blocking);
    const auto poll = [&kernel](KernelCallContext& c) { c.ret(kernel.poll_semaphore(c.sarg(0))); };
    add("PollSema", poll);
    add("iPollSema", poll, irq_ok);
    const auto refer_sema = [&kernel](KernelCallContext& c) {
        const Semaphore* s = kernel.semaphore(c.sarg(0));
        if (s == nullptr) {
            c.ret(kabi::unknown_semaphore_id);
            return;
        }
        const std::uint32_t info = c.arg(1);
        if (info == 0 || !guest_block_ok(info, kabi::sema_param::size)) {
            c.ret(kabi::error);
            return;
        }
        GuestMemory& mem = c.mem();
        namespace sp = kabi::sema_param;
        write_u32(mem, info + sp::count, static_cast<std::uint32_t>(s->count));
        write_u32(mem, info + sp::max_count, static_cast<std::uint32_t>(s->max_count));
        write_u32(mem, info + sp::init_count, static_cast<std::uint32_t>(s->init_count));
        write_u32(mem, info + sp::wait_threads, static_cast<std::uint32_t>(s->waiters.size()));
        write_u32(mem, info + sp::attr, s->attr);
        write_u32(mem, info + sp::option, s->option);
        c.ret(kabi::ok);
    };
    add("ReferSemaStatus", refer_sema);
    add("iReferSemaStatus", refer_sema, irq_ok);

    // ---- Event flags.
    add("CreateEventFlag", [&kernel](KernelCallContext& c) {
        const std::uint32_t param = c.arg(0);
        if (param == 0 || !guest_block_ok(param, kabi::event_flag_param::size)) {
            c.ret(kabi::error);
            return;
        }
        GuestMemory& mem = c.mem();
        namespace ep = kabi::event_flag_param;
        c.ret(kernel.create_event_flag(read_u32(mem, param + ep::init_bits), read_u32(mem, param + ep::attr),
                                       read_u32(mem, param + ep::option)));
    });
    add("DeleteEventFlag", [&kernel](KernelCallContext& c) { c.ret(kernel.delete_event_flag(c.sarg(0))); });
    const auto set_flag = [&kernel](KernelCallContext& c) {
        c.ret(kernel.set_event_flag(c.sarg(0), c.arg(1)));
    };
    add("SetEventFlag", set_flag);
    add("iSetEventFlag", set_flag, irq_ok);
    const auto clear_flag = [&kernel](KernelCallContext& c) {
        c.ret(kernel.clear_event_flag(c.sarg(0), c.arg(1)));
    };
    add("ClearEventFlag", clear_flag);
    add("iClearEventFlag", clear_flag, irq_ok);
    add(
        "WaitEventFlag",
        [&kernel](KernelCallContext& c) {
            const std::int32_t check = check_event_wait(kernel, c.sarg(0), c.arg(1), c.arg(2), c.arg(3));
            c.ret(check != kabi::ok ? check
                                    : kernel.wait_event_flag(c.sarg(0), c.arg(1), c.arg(2), c.arg(3)));
        },
        blocking);
    const auto poll_flag = [&kernel](KernelCallContext& c) {
        const std::int32_t check = check_event_wait(kernel, c.sarg(0), c.arg(1), c.arg(2), c.arg(3));
        if (check != kabi::ok) {
            c.ret(check);
            return;
        }
        std::uint32_t observed = 0;
        const std::int32_t result = kernel.poll_event_flag(c.sarg(0), c.arg(1), c.arg(2), observed);
        if (result == kabi::ok && c.arg(3) != 0) {
            write_u32(c.mem(), c.arg(3), observed);
        }
        c.ret(result);
    };
    add("PollEventFlag", poll_flag);
    add("iPollEventFlag", poll_flag, irq_ok);
    const auto refer_flag = [&kernel](KernelCallContext& c) {
        const EventFlag* f = kernel.event_flag(c.sarg(0));
        if (f == nullptr) {
            c.ret(kabi::unknown_event_flag_id);
            return;
        }
        const std::uint32_t info = c.arg(1);
        if (info == 0 || !guest_block_ok(info, kabi::event_flag_param::status_size)) {
            c.ret(kabi::error);
            return;
        }
        GuestMemory& mem = c.mem();
        namespace ep = kabi::event_flag_param;
        write_u32(mem, info + ep::attr, f->attr);
        write_u32(mem, info + ep::option, f->option);
        write_u32(mem, info + ep::init_bits, f->init_bits);
        write_u32(mem, info + ep::current_bits, f->bits);
        write_u32(mem, info + ep::wait_threads, static_cast<std::uint32_t>(f->waiters.size()));
        write_u32(mem, info + ep::wait_threads + 4, 0);
        write_u32(mem, info + ep::wait_threads + 8, 0);
        c.ret(kabi::ok);
    };
    add("ReferEventFlagStatus", refer_flag);
    add("iReferEventFlagStatus", refer_flag, irq_ok);

    // ---- Alarms: SetAlarm(ticks, handler, arg); the handler runs with the caller's $gp.
    const auto set_alarm = [&kernel](KernelCallContext& c) {
        c.ret(kernel.set_alarm(c.arg(0) & 0xFFFFu, c.arg(1), c.arg(2), gpr_u32(c.ctx(), 28)));
    };
    add("SetAlarm", set_alarm);
    add("iSetAlarm", set_alarm, irq_ok);
    const auto cancel_alarm = [&kernel](KernelCallContext& c) { c.ret(kernel.cancel_alarm(c.sarg(0))); };
    add("CancelAlarm", cancel_alarm);
    add("iCancelAlarm", cancel_alarm, irq_ok);
    // The SDK library spellings of the same calls.
    services.alias("ReleaseAlarm", "CancelAlarm");
    services.alias("iReleaseAlarm", "iCancelAlarm", true);
}

} // namespace dq8::rt
