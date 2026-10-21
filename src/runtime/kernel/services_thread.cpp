// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xRuntime/src/lib/Kernel/Syscalls/Thread.cpp, re-signed to KernelCallContext. What changed: no exit
// handlers (the EE kernel has none; upstream emulates a library feature), StartThread stops for a thread
// without a stack instead of allocating one, ReferThreadStatus with a null block returns the status code.
#include "dq8/abi/ps2recomp/ps2_address.h"
#include "runtime/ee/context.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/kernel/ee_kernel.h"
#include "runtime/kernel/ee_services.h"
#include "runtime/kernel/kernel_abi.h"
#include "runtime/kernel/kernel_services.h"

#include <cstdint>
#include <string>
#include <utility>

namespace dq8::rt {

namespace detail {

bool guest_block_ok(std::uint32_t address, std::uint32_t size, std::uint32_t align) noexcept {
    if (align != 0 && (address & (align - 1)) != 0) {
        return false;
    }
    if (ps2_is_scratchpad_address(address)) {
        const std::uint32_t offset = address - PS2_SCRATCHPAD_BASE;
        return size <= PS2_SCRATCHPAD_SIZE && offset <= PS2_SCRATCHPAD_SIZE - size;
    }
    std::uint32_t offset = 0;
    if (address < PS2_RAM_SIZE) {
        offset = address;
    } else if ((address >= 0x20000000u && address < 0x40000000u) ||
               (address >= 0x80000000u && address < 0xC0000000u)) {
        offset = address & 0x1FFFFFFFu;
    } else {
        return false;
    }
    return size <= PS2_RAM_SIZE && offset <= PS2_RAM_SIZE - size;
}

std::uint32_t read_u32(GuestMemory& memory, std::uint32_t address) {
    return memory.load32(address);
}

void write_u32(GuestMemory& memory, std::uint32_t address, std::uint32_t value) {
    memory.store32(address, value);
}

} // namespace detail

namespace {

using detail::guest_block_ok;
using detail::read_u32;
using detail::write_u32;

std::int32_t raw_status(ThreadStatus status) noexcept {
    switch (status) {
    case ThreadStatus::running:
        return kabi::status_run;
    case ThreadStatus::ready:
        return kabi::status_ready;
    case ThreadStatus::waiting:
        return kabi::status_wait;
    case ThreadStatus::waiting_suspended:
        return kabi::status_wait_suspend;
    case ThreadStatus::suspended:
        return kabi::status_suspend;
    case ThreadStatus::dormant:
        return kabi::status_dormant;
    }
    return kabi::status_dormant;
}

std::uint32_t raw_wait_type(WaitKind kind) noexcept {
    switch (kind) {
    case WaitKind::sleep:
        return kabi::wait_sleep;
    case WaitKind::semaphore:
        return kabi::wait_semaphore;
    case WaitKind::event_flag:
        return kabi::wait_event_flag;
    case WaitKind::none:
        return kabi::wait_none;
    }
    return kabi::wait_none;
}

} // namespace

void register_thread_services(KernelServices& services, EeKernel& kernel) {
    const auto add = [&services](std::string name, KernelServiceFn fn, std::uint32_t flags = 0) {
        services.add(KernelServiceInfo{std::move(name), std::move(fn), flags, {}, "kernel"});
    };
    constexpr std::uint32_t blocking = kernel_service_blocking;
    constexpr std::uint32_t irq_ok = kernel_service_interrupt_ok;

    add("CreateThread", [&kernel](KernelCallContext& c) {
        const std::uint32_t param = c.arg(0);
        if (param == 0 || !guest_block_ok(param, kabi::thread_param::size)) {
            c.ret(kabi::error);
            return;
        }
        GuestMemory& mem = c.mem();
        ThreadParams p;
        p.entry = read_u32(mem, param + kabi::thread_param::entry);
        p.stack = read_u32(mem, param + kabi::thread_param::stack);
        const auto stack_size =
            static_cast<std::int32_t>(read_u32(mem, param + kabi::thread_param::stack_size));
        p.gp = read_u32(mem, param + kabi::thread_param::gp);
        // A new thread starts at its initial priority; the current-priority field is status-only.
        p.priority = static_cast<std::int32_t>(read_u32(mem, param + kabi::thread_param::initial_priority));
        p.attr = read_u32(mem, param + kabi::thread_param::attr);
        p.option = read_u32(mem, param + kabi::thread_param::option);
        if (stack_size < 0 ||
            (p.stack != 0 && !guest_block_ok(p.stack, static_cast<std::uint32_t>(stack_size), 1))) {
            c.ret(kabi::error);
            return;
        }
        p.stack_size = static_cast<std::uint32_t>(stack_size);
        c.ret(kernel.create_thread(p));
    });
    add("DeleteThread", [&kernel](KernelCallContext& c) { c.ret(kernel.delete_thread(c.sarg(0))); });
    add("StartThread", [&kernel](KernelCallContext& c) {
        const GuestThread* t = kernel.thread(c.sarg(0));
        if (t != nullptr && t->status == ThreadStatus::dormant && t->stack == 0) {
            c.unimplemented(
                "StartThread of a thread created without a stack (the kernel would have to allocate one)");
        }
        c.ret(kernel.start_thread(c.sarg(0), c.arg(1), c.ctx()));
    });
    add("ExitThread", [&kernel](KernelCallContext&) { kernel.exit_current(false); }, blocking);
    add("ExitDeleteThread", [&kernel](KernelCallContext&) { kernel.exit_current(true); }, blocking);
    add(
        "TerminateThread", [&kernel](KernelCallContext& c) { c.ret(kernel.terminate_thread(c.sarg(0))); },
        irq_ok);
    add(
        "SuspendThread", [&kernel](KernelCallContext& c) { c.ret(kernel.suspend_thread(c.sarg(0))); },
        blocking | irq_ok);
    add("ResumeThread", [&kernel](KernelCallContext& c) { c.ret(kernel.resume_thread(c.sarg(0))); }, irq_ok);
    add("GetThreadId", [&kernel](KernelCallContext& c) { c.ret(kernel.current_thread_id()); }, irq_ok);

    const auto refer = [&kernel](KernelCallContext& c) {
        std::int32_t id = c.sarg(0);
        if (id == 0) {
            id = kernel.current_thread_id();
        }
        const GuestThread* t = kernel.thread(id);
        if (t == nullptr) {
            c.ret(kabi::unknown_thread_id);
            return;
        }
        const std::uint32_t info = c.arg(1);
        if (info == 0) {
            c.ret(raw_status(t->status));
            return;
        }
        if (!guest_block_ok(info, kabi::thread_param::status_size)) {
            c.ret(kabi::error);
            return;
        }
        GuestMemory& mem = c.mem();
        namespace tp = kabi::thread_param;
        write_u32(mem, info + tp::status, static_cast<std::uint32_t>(raw_status(t->status)));
        write_u32(mem, info + tp::entry, t->entry);
        write_u32(mem, info + tp::stack, t->stack);
        write_u32(mem, info + tp::stack_size, t->stack_size);
        write_u32(mem, info + tp::gp, t->gp);
        write_u32(mem, info + tp::initial_priority, static_cast<std::uint32_t>(t->initial_priority));
        write_u32(mem, info + tp::current_priority, static_cast<std::uint32_t>(t->current_priority));
        write_u32(mem, info + tp::attr, t->attr);
        write_u32(mem, info + tp::option, t->option);
        write_u32(mem, info + tp::wait_type, raw_wait_type(t->wait.kind));
        write_u32(mem, info + tp::wait_id, static_cast<std::uint32_t>(t->wait.object));
        write_u32(mem, info + tp::wakeup_count, t->wakeup_count);
        c.ret(kabi::ok);
    };
    add("ReferThreadStatus", refer);
    add("iReferThreadStatus", refer, irq_ok);

    add("SleepThread", [&kernel](KernelCallContext& c) { c.ret(kernel.sleep_current()); }, blocking);
    const auto wakeup = [&kernel](KernelCallContext& c) { c.ret(kernel.wakeup_thread(c.sarg(0))); };
    add("WakeupThread", wakeup);
    add("iWakeupThread", wakeup, irq_ok);
    add("CancelWakeupThread", [&kernel](KernelCallContext& c) { c.ret(kernel.cancel_wakeup(c.sarg(0))); });
    add(
        "iCancelWakeupThread",
        [&kernel](KernelCallContext& c) {
            c.ret(c.arg(0) == 0 ? kabi::illegal_thread_id : kernel.cancel_wakeup(c.sarg(0)));
        },
        irq_ok);
    // ChangeThreadPriority returns KE_OK as upstream does; whether the EE kernel returns the previous
    // priority is to be confirmed in bring-up.
    const auto change = [&kernel](KernelCallContext& c) {
        std::int32_t old = 0;
        c.ret(kernel.change_priority(c.sarg(0), c.sarg(1), old));
    };
    add("ChangeThreadPriority", change);
    add("iChangeThreadPriority", change, irq_ok);
    const auto rotate = [&kernel](KernelCallContext& c) { c.ret(kernel.rotate_ready_queue(c.sarg(0))); };
    add("RotateThreadReadyQueue", rotate, blocking);
    add("iRotateThreadReadyQueue", rotate, irq_ok);
    const auto release = [&kernel](KernelCallContext& c) { c.ret(kernel.release_wait(c.sarg(0))); };
    add("ReleaseWaitThread", release);
    add("iReleaseWaitThread", release, irq_ok);
}

} // namespace dq8::rt
