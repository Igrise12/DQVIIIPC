// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/iop_exec.h"
#include "runtime/iop/kernel/hle.h"

#include <cstdint>

// thbase, thsemap and thevent as HLE (Docs/milestones/M4-playable.md, session A; the census ordinals; the
// structures they read and write are in "Protocol notes"). A blocking call from the interrupt context returns
// KE_ILLEGAL_CONTEXT, and one made with interrupts suspended KE_CAN_NOT_WAIT, as the IOP kernel answers.
namespace dq8::rt {

namespace {

// The IOP system clock: 36.864 MHz.
constexpr std::uint64_t ticks_per_ms = 36'864;
constexpr std::uint32_t sema_attr_priority = 0x01; // SA_THPRI: waiters by priority
constexpr std::uint32_t evf_attr_multi = 0x02;     // EA_MULTI: more than one waiter
constexpr std::uint32_t evf_mode_mask = 0x31;

// The thread a call names: 0 is the caller (none in the interrupt context).
IopThread* named(HleCall& c, std::uint32_t thid) {
    if (thid == 0) {
        return c.thread();
    }
    return c.exec().thread(static_cast<std::int32_t>(thid));
}

// The checks every blocking call makes; false when it already returned an error.
bool can_block(HleCall& c) {
    if (c.in_interrupt()) {
        c.ret(iop_error::illegal_context);
        return false;
    }
    if (!c.thread()->interrupts) {
        c.ret(iop_error::can_not_wait);
        return false;
    }
    return true;
}

// ---- thbase

void create_thread(HleCall& c) {
    if (c.in_interrupt()) {
        c.ret(iop_error::illegal_context);
        return;
    }
    const std::uint32_t p = c.arg(0);
    const std::uint32_t attr = c.read32(p + 0x00);
    const std::uint32_t option = c.read32(p + 0x04);
    const std::uint32_t entry = c.read32(p + 0x08);
    const std::uint32_t stack = c.read32(p + 0x0C);
    const auto priority = static_cast<std::int32_t>(c.read32(p + 0x10));
    c.ret(c.exec().create_thread(attr, option, entry, stack, priority, c.caller_gp(),
                                 c.exec().module_at(c.cpu().gpr[31])));
}

void delete_thread(HleCall& c) {
    const std::uint32_t thid = c.arg(0);
    if (thid == 0 || (c.thread() != nullptr && static_cast<std::int32_t>(thid) == c.thread()->id)) {
        c.ret(iop_error::illegal_thid);
        return;
    }
    IopThread* t = c.exec().thread(static_cast<std::int32_t>(thid));
    if (t == nullptr) {
        c.ret(iop_error::unknown_thid);
        return;
    }
    if (t->status != IopThreadStatus::dormant) {
        c.ret(iop_error::not_dormant);
        return;
    }
    c.exec().delete_thread(*t);
    c.ret(0);
}

void start_thread(HleCall& c) {
    IopThread* t = c.exec().thread(static_cast<std::int32_t>(c.arg(0)));
    if (c.arg(0) == 0) {
        c.ret(iop_error::illegal_thid);
        return;
    }
    if (t == nullptr) {
        c.ret(iop_error::unknown_thid);
        return;
    }
    if (t->status != IopThreadStatus::dormant) {
        c.ret(iop_error::not_dormant);
        return;
    }
    c.exec().start_thread(*t, c.arg(1));
    c.ret(0);
}

void terminate_thread(HleCall& c) {
    const std::uint32_t thid = c.arg(0);
    if (thid == 0 || (c.thread() != nullptr && static_cast<std::int32_t>(thid) == c.thread()->id)) {
        c.ret(iop_error::illegal_thid);
        return;
    }
    IopThread* t = c.exec().thread(static_cast<std::int32_t>(thid));
    if (t == nullptr) {
        c.ret(iop_error::unknown_thid);
        return;
    }
    if (t->status == IopThreadStatus::dormant) {
        c.ret(iop_error::dormant);
        return;
    }
    c.exec().terminate_thread(*t);
    c.ret(0);
}

void change_thread_priority(HleCall& c) {
    IopThread* t = named(c, c.arg(0));
    auto priority = static_cast<std::int32_t>(c.arg(1));
    if (t == nullptr) {
        c.ret(c.arg(0) == 0 ? iop_error::illegal_context : iop_error::unknown_thid);
        return;
    }
    if (priority == 0 && c.thread() != nullptr) {
        priority = c.thread()->priority; // the caller's own priority
    }
    if (priority < 1 || priority > 126) {
        c.ret(iop_error::illegal_priority);
        return;
    }
    if (t->status == IopThreadStatus::dormant) {
        c.ret(iop_error::dormant);
        return;
    }
    c.exec().change_priority(*t, priority);
    c.ret(0);
}

void get_thread_id(HleCall& c) {
    if (c.in_interrupt()) {
        c.ret(iop_error::illegal_context);
        return;
    }
    c.ret(c.thread()->id);
}

void refer_thread_status(HleCall& c) {
    IopThread* t = named(c, c.arg(0));
    if (t == nullptr) {
        c.ret(c.arg(0) == 0 ? iop_error::illegal_context : iop_error::unknown_thid);
        return;
    }
    c.exec().write_thread_status(*t, c.arg(1), c);
    c.ret(0);
}

void sleep_thread(HleCall& c) {
    if (!can_block(c)) {
        return;
    }
    IopThread& t = *c.thread();
    c.ret(0);
    if (t.wakeups > 0) {
        --t.wakeups;
        return;
    }
    c.exec().block(t, IopWait::sleep, 0);
}

void i_wakeup_thread(HleCall& c) {
    if (c.arg(0) == 0) {
        c.ret(iop_error::illegal_thid);
        return;
    }
    IopThread* t = c.exec().thread(static_cast<std::int32_t>(c.arg(0)));
    if (t == nullptr) {
        c.ret(iop_error::unknown_thid);
        return;
    }
    if (t->status == IopThreadStatus::dormant) {
        c.ret(iop_error::dormant);
        return;
    }
    if (t->status == IopThreadStatus::waiting && t->wait == IopWait::sleep) {
        c.exec().wake(*t, 0);
    } else {
        ++t->wakeups;
    }
    c.ret(0);
}

void i_cancel_wakeup_thread(HleCall& c) {
    IopThread* t = named(c, c.arg(0));
    if (t == nullptr) {
        c.ret(c.arg(0) == 0 ? iop_error::illegal_context : iop_error::unknown_thid);
        return;
    }
    const std::uint32_t count = t->wakeups;
    t->wakeups = 0;
    c.ret(count);
}

void delay_thread(HleCall& c) {
    if (!can_block(c)) {
        return;
    }
    c.ret(0);
    c.exec().delay(*c.thread(), cycles_from_us(c.arg(0)));
}

void get_system_time(HleCall& c) {
    const std::uint64_t now = c.exec().sysclock();
    c.write32(c.arg(0), static_cast<std::uint32_t>(now));
    c.write32(c.arg(0) + 4, static_cast<std::uint32_t>(now >> 32));
    c.ret(0);
}

void i_set_alarm(HleCall& c) {
    const std::uint32_t clock = c.arg(0);
    const std::uint64_t ticks = c.read32(clock) | (std::uint64_t{c.read32(clock + 4)} << 32);
    const std::uint32_t handler = c.arg(1);
    if (handler == 0) {
        c.fail("an alarm with no handler");
    }
    c.exec().set_alarm(ticks, handler, c.arg(2), c.caller_gp());
    c.ret(0);
}

void cancel_alarm(HleCall& c) {
    c.ret(c.exec().cancel_alarm(c.arg(0), c.arg(1)) ? iop_error::ok : iop_error::notfound_handler);
}

void usec_to_sysclock(HleCall& c) {
    const std::uint64_t ticks = std::uint64_t{c.arg(0)} * ticks_per_ms / 1000;
    c.write32(c.arg(1), static_cast<std::uint32_t>(ticks));
    c.write32(c.arg(1) + 4, static_cast<std::uint32_t>(ticks >> 32));
    c.ret(0);
}

void sysclock_to_usec(HleCall& c) {
    const std::uint32_t clock = c.arg(0);
    const std::uint64_t ticks = c.read32(clock) | (std::uint64_t{c.read32(clock + 4)} << 32);
    const std::uint64_t us = ticks * 1000 / ticks_per_ms;
    c.write32(c.arg(1), static_cast<std::uint32_t>(us / 1'000'000));
    c.write32(c.arg(2), static_cast<std::uint32_t>(us % 1'000'000));
    c.ret(0);
}

// ---- thsemap

void create_sema(HleCall& c) {
    const std::uint32_t p = c.arg(0);
    IopSema s;
    s.attr = c.read32(p + 0x00);
    s.option = c.read32(p + 0x04);
    s.count = static_cast<std::int32_t>(c.read32(p + 0x08));
    s.max = static_cast<std::int32_t>(c.read32(p + 0x0C));
    s.id = c.exec().next_object_id();
    const std::int32_t id = s.id;
    c.exec().semas().emplace(id, std::move(s));
    c.ret(id);
}

IopSema* sema(HleCall& c) {
    const auto it = c.exec().semas().find(static_cast<std::int32_t>(c.arg(0)));
    if (it == c.exec().semas().end()) {
        c.ret(iop_error::unknown_semid);
        return nullptr;
    }
    return &it->second;
}

void delete_sema(HleCall& c) {
    IopSema* s = sema(c);
    if (s == nullptr) {
        return;
    }
    const std::deque<std::int32_t> waiters = s->waiters;
    const std::int32_t id = s->id;
    for (const std::int32_t w : waiters) {
        if (IopThread* t = c.exec().thread(w)) {
            c.exec().wake(*t, iop_error::wait_delete);
        }
    }
    c.exec().semas().erase(id);
    c.ret(0);
}

void signal_sema(HleCall& c) {
    IopSema* s = sema(c);
    if (s == nullptr) {
        return;
    }
    if (!s->waiters.empty()) {
        std::int32_t chosen = s->waiters.front();
        if ((s->attr & sema_attr_priority) != 0) {
            std::int32_t best = 1000;
            for (const std::int32_t w : s->waiters) {
                const IopThread* t = c.exec().thread(w);
                if (t != nullptr && t->priority < best) {
                    best = t->priority;
                    chosen = w;
                }
            }
        }
        c.exec().wake(*c.exec().thread(chosen), 0);
    } else if (s->count >= s->max) {
        c.ret(iop_error::sema_ovf);
        return;
    } else {
        ++s->count;
    }
    c.ret(0);
}

void wait_sema(HleCall& c) {
    if (!can_block(c)) {
        return;
    }
    IopSema* s = sema(c);
    if (s == nullptr) {
        return;
    }
    c.ret(0);
    if (s->count > 0) {
        --s->count;
        return;
    }
    s->waiters.push_back(c.thread()->id);
    c.exec().block(*c.thread(), IopWait::sema, static_cast<std::uint32_t>(s->id));
}

// ---- thevent

void create_event_flag(HleCall& c) {
    const std::uint32_t p = c.arg(0);
    IopEventFlag f;
    f.attr = c.read32(p + 0x00);
    f.option = c.read32(p + 0x04);
    f.bits = c.read32(p + 0x08);
    f.id = c.exec().next_object_id();
    const std::int32_t id = f.id;
    c.exec().event_flags().emplace(id, std::move(f));
    c.ret(id);
}

IopEventFlag* event_flag(HleCall& c) {
    const auto it = c.exec().event_flags().find(static_cast<std::int32_t>(c.arg(0)));
    if (it == c.exec().event_flags().end()) {
        c.ret(iop_error::unknown_evfid);
        return nullptr;
    }
    return &it->second;
}

void delete_event_flag(HleCall& c) {
    IopEventFlag* f = event_flag(c);
    if (f == nullptr) {
        return;
    }
    const std::deque<std::int32_t> waiters = f->waiters;
    const std::int32_t id = f->id;
    for (const std::int32_t w : waiters) {
        if (IopThread* t = c.exec().thread(w)) {
            c.exec().wake(*t, iop_error::wait_delete);
        }
    }
    c.exec().event_flags().erase(id);
    c.ret(0);
}

void set_event_flag(HleCall& c) {
    IopEventFlag* f = event_flag(c);
    if (f == nullptr) {
        return;
    }
    f->bits |= c.arg(1);
    c.exec().check_event_flag(*f);
    c.ret(0);
}

void clear_event_flag(HleCall& c) {
    IopEventFlag* f = event_flag(c);
    if (f == nullptr) {
        return;
    }
    f->bits &= c.arg(1);
    c.ret(0);
}

void wait_event_flag(HleCall& c) {
    if (c.in_interrupt()) {
        c.ret(iop_error::illegal_context);
        return;
    }
    const std::uint32_t bits = c.arg(1);
    const std::uint32_t mode = c.arg(2);
    const std::uint32_t result = c.arg(3);
    if (bits == 0) {
        c.ret(iop_error::evf_ilpat);
        return;
    }
    if ((mode & ~evf_mode_mask) != 0) {
        c.ret(iop_error::illegal_mode);
        return;
    }
    IopEventFlag* f = event_flag(c);
    if (f == nullptr) {
        return;
    }
    if ((f->attr & evf_attr_multi) == 0 && !f->waiters.empty()) {
        c.ret(iop_error::evf_multi);
        return;
    }
    if (IopExec::event_flag_matches(f->bits, bits, mode)) {
        if (result != 0) {
            c.write32(result, f->bits);
        }
        IopExec::consume_event_flag(*f, bits, mode);
        c.ret(0);
        return;
    }
    if (!c.thread()->interrupts) {
        c.ret(iop_error::can_not_wait);
        return;
    }
    IopThread& t = *c.thread();
    t.evf_bits = bits;
    t.evf_mode = mode;
    t.evf_result = result;
    f->waiters.push_back(t.id);
    c.ret(0);
    c.exec().block(t, IopWait::event_flag, static_cast<std::uint32_t>(f->id));
}

constexpr HleFunction thbase_table[] = {
    {"thbase", 4, "CreateThread", create_thread},
    {"thbase", 5, "DeleteThread", delete_thread},
    {"thbase", 6, "StartThread", start_thread},
    {"thbase", 10, "TerminateThread", terminate_thread},
    {"thbase", 14, "ChangeThreadPriority", change_thread_priority},
    {"thbase", 20, "GetThreadId", get_thread_id},
    {"thbase", 22, "ReferThreadStatus", refer_thread_status},
    {"thbase", 24, "SleepThread", sleep_thread},
    {"thbase", 26, "iWakeupThread", i_wakeup_thread},
    {"thbase", 28, "iCancelWakeupThread", i_cancel_wakeup_thread},
    {"thbase", 33, "DelayThread", delay_thread},
    {"thbase", 34, "GetSystemTime", get_system_time},
    {"thbase", 36, "iSetAlarm", i_set_alarm},
    {"thbase", 37, "CancelAlarm", cancel_alarm},
    {"thbase", 38, "iCancelAlarm", cancel_alarm},
    {"thbase", 39, "USec2SysClock", usec_to_sysclock},
    {"thbase", 40, "SysClock2USec", sysclock_to_usec},
};

constexpr HleFunction thsemap_table[] = {
    {"thsemap", 4, "CreateSema", create_sema}, {"thsemap", 5, "DeleteSema", delete_sema},
    {"thsemap", 6, "SignalSema", signal_sema}, {"thsemap", 7, "iSignalSema", signal_sema},
    {"thsemap", 8, "WaitSema", wait_sema},
};

constexpr HleFunction thevent_table[] = {
    {"thevent", 4, "CreateEventFlag", create_event_flag},
    {"thevent", 5, "DeleteEventFlag", delete_event_flag},
    {"thevent", 6, "SetEventFlag", set_event_flag},
    {"thevent", 7, "iSetEventFlag", set_event_flag},
    {"thevent", 8, "ClearEventFlag", clear_event_flag},
    {"thevent", 9, "iClearEventFlag", clear_event_flag},
    {"thevent", 10, "WaitEventFlag", wait_event_flag},
};

} // namespace

std::span<const HleFunction> hle_thbase() {
    return thbase_table;
}

std::span<const HleFunction> hle_thsemap() {
    return thsemap_table;
}

std::span<const HleFunction> hle_thevent() {
    return thevent_table;
}

} // namespace dq8::rt
