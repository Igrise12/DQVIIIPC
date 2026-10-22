// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/iop/cpu/iop_cpu.h"
#include "runtime/machine/events.h"

#include <cstdint>
#include <deque>
#include <string>

// The HLE IOP kernel's objects (Docs/milestones/M4-playable.md, session A; "Protocol notes" has the guest
// structures they are read from and written to). Threads are register contexts scheduled by priority (a lower
// number runs first, no time slicing); every wait is a host-side state plus, for time waits, an EventQueue
// event. Ids count up from 1 per IOP boot and are never reused within it.
namespace dq8::rt {

enum class IopThreadStatus : std::uint8_t { dormant, ready, running, waiting };

// What a waiting thread waits for. The numbers are the waitType ReferThreadStatus reports.
enum class IopWait : std::uint8_t {
    none = 0,
    sleep = 1,      // SleepThread
    delay = 2,      // DelayThread
    sema = 3,       // WaitSema
    event_flag = 4, // WaitEventFlag
    rpc_server = 5, // sceSifRpcLoop with no request queued (reported as sleep)
    rpc_client = 6, // sceSifBindRpc / sceSifCallRpc in wait mode (reported as sleep)
};

// ReferThreadStatus status bits.
namespace iop_thread_status {
inline constexpr std::uint32_t run = 0x01;
inline constexpr std::uint32_t ready = 0x02;
inline constexpr std::uint32_t wait = 0x04;
inline constexpr std::uint32_t dormant = 0x10;
} // namespace iop_thread_status

// What the return trap does when a guest function an HLE routine called returns in a thread.
enum class IopResume : std::uint8_t { none, module_start, rpc_loop };

struct IopThread {
    std::int32_t id = 0;
    std::uint32_t attr = 0;
    std::uint32_t option = 0;
    std::uint32_t entry = 0;
    std::uint32_t stack = 0; // lowest address of the stack block
    std::uint32_t stack_size = 0;
    std::uint32_t gp = 0;
    std::int32_t init_priority = 0;
    std::int32_t priority = 0;
    IopThreadStatus status = IopThreadStatus::dormant;
    IopWait wait = IopWait::none;
    std::uint32_t wait_id = 0;
    std::uint32_t wakeups = 0;
    IopCpuState cpu;
    bool interrupts = true; // CpuSuspendIntr / CpuResumeIntr
    // WaitEventFlag's pattern, mode and result address.
    std::uint32_t evf_bits = 0;
    std::uint32_t evf_mode = 0;
    std::uint32_t evf_result = 0;
    EventId delay_event = 0;
    // A guest call made on this thread's behalf, and where its return goes.
    IopResume resume = IopResume::none;
    std::uint32_t resume_arg = 0;   // the RPC queue (rpc_loop), the module index (module_start)
    std::uint32_t loop_sp = 0;      // sceSifRpcLoop's caller's stack pointer
    std::uint64_t request = 0;      // the RPC request being served
    std::uint32_t request_recv = 0; // its reply size
    std::string module;             // the module whose code created it
    bool start_thread = false;      // a module-start thread the loader created and deletes
};

struct IopSema {
    std::int32_t id = 0;
    std::uint32_t attr = 0;
    std::uint32_t option = 0;
    std::int32_t count = 0;
    std::int32_t max = 0;
    std::deque<std::int32_t> waiters;
};

struct IopEventFlag {
    std::int32_t id = 0;
    std::uint32_t attr = 0;
    std::uint32_t option = 0;
    std::uint32_t bits = 0;
    std::deque<std::int32_t> waiters;
};

// Work that runs in the interrupt context, one item at a time: a guest function called with one argument
// and a return trap.
struct IopIrqJob {
    enum class Kind : std::uint8_t { intr_handler, timer, alarm, rpc_end };
    Kind kind = Kind::intr_handler;
    std::uint32_t function = 0;
    std::uint32_t arg = 0;
    std::uint32_t gp = 0;
    std::uint32_t ref = 0; // the interrupt number, timer index or alarm id
};

struct IopIntrHandler {
    std::uint32_t mode = 0;
    std::uint32_t function = 0;
    std::uint32_t arg = 0;
    std::uint32_t gp = 0;
};

struct IopAlarm {
    std::uint32_t id = 0;
    std::uint32_t handler = 0;
    std::uint32_t common = 0;
    std::uint32_t gp = 0;
    Cycles due = 0;
    EventId event = 0;
    bool running = false; // its handler is queued or executing
};

// One of the six IOP hardware timers as timrman hands them out; pure HLE: the counter is computed from time.
struct IopHardTimer {
    std::uint32_t users = 0;
    std::uint32_t source = 0;
    std::uint32_t prescale = 1;
    std::uint32_t mode = 0;
    bool configured = false;
    bool running = false;
    std::uint32_t compare = 0;
    std::uint32_t handler = 0;
    std::uint32_t common = 0;
    std::uint32_t gp = 0;
    Cycles started = 0; // when the counter was last 0
    EventId event = 0;
};

// A server an interpreted module registered with sceSifRegisterRpc.
struct IopGuestServer {
    std::uint32_t sid = 0;
    std::uint32_t data = 0; // the guest's server structure: the handle the EE binds to
    std::uint32_t function = 0;
    std::uint32_t buffer = 0;
    std::uint32_t cfunction = 0;
    std::uint32_t cbuffer = 0;
    std::uint32_t queue = 0;
    std::uint32_t gp = 0;
    std::string module;
};

// An EE call waiting for a guest server's thread.
struct IopGuestRequest {
    std::uint64_t request = 0; // IopRpcLayer's request number
    std::uint32_t server = 0;  // the server structure
    std::uint32_t fno = 0;
    std::uint32_t send_size = 0;
    std::uint32_t recv_size = 0;
};

struct IopRpcQueue {
    std::uint32_t address = 0;
    std::int32_t thread = 0;
    std::deque<IopGuestRequest> pending;
};

// The IOP as an RPC client of an EE server (bind or call in flight).
struct IopClientOp {
    bool bind = false;
    std::uint32_t rpc_id = 0;
    std::uint32_t mode = 0;
    std::int32_t thread = 0; // a waiting thread, 0 for none
    std::uint32_t end_function = 0;
    std::uint32_t end_param = 0;
    std::uint32_t gp = 0;
};

} // namespace dq8::rt
