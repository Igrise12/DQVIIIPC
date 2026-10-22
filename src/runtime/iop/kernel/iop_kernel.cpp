// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/iop_exec.h"
#include "runtime/iop/iop_memory.h"
#include "runtime/iop/kernel/hle.h"
#include "runtime/machine/cost_model.h"
#include "runtime/machine/machine_services.h"

#include <algorithm>
#include <vector>

// The HLE kernel's object algorithms: sysmem's allocator, threads and their waits, event flag matching and
// the alarms of thbase. The HLE functions (hle_*.cpp) check arguments and call these.
namespace dq8::rt {

namespace {

constexpr std::uint32_t sysmem_unit = 0x100;
constexpr std::uint32_t th_no_fillstack = 0x00100000u;

void erase_id(std::deque<std::int32_t>& q, std::int32_t id) {
    q.erase(std::remove(q.begin(), q.end(), id), q.end());
}

} // namespace

std::uint32_t IopExec::alloc(std::uint32_t mode, std::uint32_t size, std::uint32_t address) {
    std::optional<std::uint32_t> block;
    switch (mode) {
    case 0:
        block = ram_.allocate(size, sysmem_unit, false);
        break;
    case 1:
        block = ram_.allocate(size, sysmem_unit, true);
        break;
    case 2:
        block = ram_.allocate_at(iop_physical(address) & (iop_ram_bytes - 1), size, sysmem_unit);
        break;
    default:
        return 0;
    }
    return block.value_or(0);
}

bool IopExec::free(std::uint32_t address) {
    return ram_.free(iop_physical(address) & (iop_ram_bytes - 1));
}

IopThread* IopExec::thread(std::int32_t id) {
    const auto it = threads_.find(id);
    return it == threads_.end() ? nullptr : &it->second;
}

IopThread* IopExec::current_thread() {
    return in_irq_ ? nullptr : thread(current_);
}

std::int32_t IopExec::create_thread(std::uint32_t attr, std::uint32_t option, std::uint32_t entry,
                                    std::uint32_t stack_size, std::int32_t priority, std::uint32_t gp,
                                    std::string module) {
    if (priority < 1 || priority > 126) {
        return iop_error::illegal_priority;
    }
    if (entry == 0 || (entry & 3u) != 0) {
        return iop_error::illegal_entry;
    }
    if (stack_size == 0 || stack_size > iop_ram_bytes) {
        return iop_error::illegal_stacksize;
    }
    const std::uint32_t size = (stack_size + sysmem_unit - 1) & ~(sysmem_unit - 1);
    const std::uint32_t stack = alloc(1, size, 0);
    if (stack == 0) {
        return iop_error::no_memory;
    }
    if ((attr & th_no_fillstack) == 0) {
        ram_.write(stack, std::vector<std::uint8_t>(size, 0xFF));
    }
    IopThread t;
    t.id = next_thread_id_++;
    t.attr = attr;
    t.option = option;
    t.entry = entry;
    t.stack = stack;
    t.stack_size = size;
    t.gp = gp;
    t.init_priority = priority;
    t.priority = priority;
    t.module = std::move(module);
    const std::int32_t id = t.id;
    threads_.emplace(id, std::move(t));
    machine_.trace(TraceCategory::iop, "create thread " + std::to_string(id) + " entry " + hex32(entry) +
                                           " priority " + std::to_string(priority));
    return id;
}

void IopExec::start_thread(IopThread& t, std::uint32_t arg) {
    t.cpu = IopCpuState{};
    t.cpu.gpr[4] = arg;
    t.cpu.gpr[28] = t.gp;
    t.cpu.gpr[29] = t.stack + t.stack_size - 16;
    t.cpu.gpr[31] = slot_address(static_cast<std::uint32_t>(SlotKind::thread_exit));
    t.cpu.pc = t.entry;
    t.priority = t.init_priority;
    t.wakeups = 0;
    t.interrupts = true;
    t.resume = IopResume::none;
    t.request = 0;
    make_ready(t);
}

void IopExec::erase_thread(std::int32_t id) {
    const auto it = threads_.find(id);
    if (it == threads_.end()) {
        return;
    }
    if (it->second.delay_event != 0) {
        machine_.events().cancel(it->second.delay_event);
    }
    free(it->second.stack);
    threads_.erase(it);
}

void IopExec::delete_thread(IopThread& t) {
    erase_thread(t.id);
}

void IopExec::remove_waiter(IopThread& t) {
    switch (t.wait) {
    case IopWait::sema:
        if (const auto it = semas_.find(static_cast<std::int32_t>(t.wait_id)); it != semas_.end()) {
            erase_id(it->second.waiters, t.id);
        }
        break;
    case IopWait::event_flag:
        if (const auto it = event_flags_.find(static_cast<std::int32_t>(t.wait_id));
            it != event_flags_.end()) {
            erase_id(it->second.waiters, t.id);
        }
        break;
    case IopWait::delay:
    case IopWait::none:
    case IopWait::sleep:
    case IopWait::rpc_server:
    case IopWait::rpc_client:
        break;
    }
    if (t.delay_event != 0) {
        machine_.events().cancel(t.delay_event);
        t.delay_event = 0;
    }
    if (t.wait == IopWait::rpc_client) {
        clients_.erase(t.wait_id);
    }
}

void IopExec::terminate_thread(IopThread& t) {
    if (t.status == IopThreadStatus::waiting) {
        remove_waiter(t);
    } else if (t.status == IopThreadStatus::ready) {
        unready(t);
    }
    if (current_ == t.id) {
        current_ = 0;
    }
    for (auto& [address, q] : rpc_queues_) {
        if (q.thread == t.id) {
            q.thread = 0;
        }
    }
    t.status = IopThreadStatus::dormant;
    t.wait = IopWait::none;
    t.resume = IopResume::none;
}

void IopExec::change_priority(IopThread& t, std::int32_t priority) {
    if (t.status == IopThreadStatus::ready) {
        unready(t);
        t.priority = priority;
        make_ready(t);
    } else {
        t.priority = priority;
    }
}

void IopExec::block(IopThread& t, IopWait wait, std::uint32_t id) {
    t.status = IopThreadStatus::waiting;
    t.wait = wait;
    t.wait_id = id;
    if (current_ == t.id) {
        current_ = 0;
    }
}

void IopExec::delay(IopThread& t, Cycles cycles) {
    block(t, IopWait::delay, 0);
    const std::int32_t id = t.id;
    t.delay_event = machine_.events().schedule(now_ + cycles, "iop_delay", [this, id](Cycles when) {
        IopThread* th = thread(id);
        if (th == nullptr) {
            return;
        }
        th->delay_event = 0;
        wake(*th, 0);
        wake_iop(when);
    });
}

void IopExec::wake(IopThread& t, std::int32_t result) {
    if (t.status != IopThreadStatus::waiting) {
        return;
    }
    remove_waiter(t);
    t.cpu.gpr[2] = static_cast<std::uint32_t>(result);
    make_ready(t);
}

void IopExec::write_thread_status(const IopThread& t, std::uint32_t info, HleCall& call) {
    std::uint32_t status = iop_thread_status::dormant;
    switch (t.status) {
    case IopThreadStatus::running:
        status = iop_thread_status::run;
        break;
    case IopThreadStatus::ready:
        status = iop_thread_status::ready;
        break;
    case IopThreadStatus::waiting:
        status = iop_thread_status::wait;
        break;
    case IopThreadStatus::dormant:
        break;
    }
    std::uint32_t wait_type = static_cast<std::uint32_t>(t.wait);
    if (t.wait == IopWait::rpc_server || t.wait == IopWait::rpc_client) {
        wait_type = static_cast<std::uint32_t>(IopWait::sleep);
    }
    const std::uint32_t fields[13] = {t.attr,
                                      t.option,
                                      status,
                                      t.entry,
                                      t.stack,
                                      t.stack_size,
                                      t.gp,
                                      static_cast<std::uint32_t>(t.init_priority),
                                      static_cast<std::uint32_t>(t.priority),
                                      t.status == IopThreadStatus::waiting ? wait_type : 0,
                                      t.status == IopThreadStatus::waiting ? t.wait_id : 0,
                                      t.wakeups,
                                      0};
    for (std::uint32_t i = 0; i < 13; ++i) {
        call.write32(info + 4 * i, fields[i]);
    }
}

bool IopExec::event_flag_matches(std::uint32_t flag, std::uint32_t bits, std::uint32_t mode) noexcept {
    return (mode & 1u) != 0 ? (flag & bits) != 0 : (flag & bits) == bits;
}

void IopExec::consume_event_flag(IopEventFlag& flag, std::uint32_t bits, std::uint32_t mode) noexcept {
    if ((mode & 0x20u) != 0) {
        flag.bits = 0;
    } else if ((mode & 0x10u) != 0) {
        flag.bits &= ~bits;
    }
}

void IopExec::check_event_flag(IopEventFlag& flag) {
    // Waiters in arrival order; each one that matches takes the bits as they are, then its clear mode
    // applies.
    std::deque<std::int32_t> waiting = flag.waiters;
    for (const std::int32_t id : waiting) {
        IopThread* t = thread(id);
        if (t == nullptr || !event_flag_matches(flag.bits, t->evf_bits, t->evf_mode)) {
            continue;
        }
        if (t->evf_result != 0) {
            const std::uint32_t at = iop_physical(t->evf_result);
            if (at < iop_ram_mirror_end) {
                ram_.write32(at & (iop_ram_bytes - 1), flag.bits);
            }
        }
        consume_event_flag(flag, t->evf_bits, t->evf_mode);
        wake(*t, 0);
    }
}

std::uint32_t IopExec::set_alarm(std::uint64_t ticks, std::uint32_t handler, std::uint32_t common,
                                 std::uint32_t gp) {
    IopAlarm a;
    a.id = next_alarm_id_++;
    a.handler = handler;
    a.common = common;
    a.gp = gp;
    a.due = now_ + cycles_of_ticks(std::max<std::uint64_t>(ticks, 1));
    const std::uint32_t id = a.id;
    a.event =
        machine_.events().schedule(a.due, "iop_alarm", [this, id](Cycles when) { alarm_due(id, when); });
    alarms_.emplace(id, a);
    return id;
}

bool IopExec::cancel_alarm(std::uint32_t handler, std::uint32_t common) {
    for (auto it = alarms_.begin(); it != alarms_.end(); ++it) {
        if (it->second.handler == handler && it->second.common == common) {
            if (it->second.event != 0) {
                machine_.events().cancel(it->second.event);
            }
            alarms_.erase(it);
            return true;
        }
    }
    return false;
}

void IopExec::alarm_due(std::uint32_t id, Cycles when) {
    const auto it = alarms_.find(id);
    if (it == alarms_.end()) {
        return;
    }
    IopAlarm& a = it->second;
    a.event = 0;
    a.due = when;
    a.running = true;
    queue_irq(IopIrqJob{IopIrqJob::Kind::alarm, a.handler, a.common, a.gp, id}, when);
}

} // namespace dq8::rt
