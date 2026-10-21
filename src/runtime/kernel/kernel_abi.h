// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xRuntime/src/lib/Kernel/Syscalls/Helpers/State.h (kernel error codes, thread status and wait codes, the
// EE thread, semaphore and event-flag parameter layouts), restated as named offset tables. The SIF structures
// of that file are deliberately not here (Docs/milestones/M3-boots.md, "Legal").
#pragma once

#include <cstdint>

// The EE kernel's guest-visible ABI: result codes, status codes and the layouts of the parameter blocks the
// thread and synchronisation calls read and write in guest memory.
namespace dq8::rt::kabi {

// Result codes ($v0).
inline constexpr std::int32_t ok = 0;
inline constexpr std::int32_t error = -1;
inline constexpr std::int32_t illegal_priority = -403;
inline constexpr std::int32_t illegal_mode = -405;
inline constexpr std::int32_t illegal_thread_id = -406;
inline constexpr std::int32_t unknown_thread_id = -407;
inline constexpr std::int32_t unknown_semaphore_id = -408;
inline constexpr std::int32_t unknown_event_flag_id = -409;
inline constexpr std::int32_t dormant = -413;
inline constexpr std::int32_t not_dormant = -414;
inline constexpr std::int32_t not_suspended = -415;
inline constexpr std::int32_t not_waiting = -416;
inline constexpr std::int32_t release_wait = -418;
inline constexpr std::int32_t semaphore_zero = -419;
inline constexpr std::int32_t semaphore_overflow = -420;
inline constexpr std::int32_t event_condition = -421;
inline constexpr std::int32_t event_multi = -422;
inline constexpr std::int32_t event_illegal_pattern = -423;
inline constexpr std::int32_t wait_deleted = -425;

// Thread status as ReferThreadStatus reports it.
inline constexpr std::int32_t status_run = 0x01;
inline constexpr std::int32_t status_ready = 0x02;
inline constexpr std::int32_t status_wait = 0x04;
inline constexpr std::int32_t status_suspend = 0x08;
inline constexpr std::int32_t status_wait_suspend = 0x0C;
inline constexpr std::int32_t status_dormant = 0x10;

// Wait types.
inline constexpr std::uint32_t wait_none = 0;
inline constexpr std::uint32_t wait_sleep = 1;
inline constexpr std::uint32_t wait_semaphore = 2;
inline constexpr std::uint32_t wait_event_flag = 3;

// WaitEventFlag / PollEventFlag modes, and the event-flag attribute that allows several waiters.
inline constexpr std::uint32_t event_mode_or = 0x01;
inline constexpr std::uint32_t event_mode_clear = 0x10;
inline constexpr std::uint32_t event_mode_clear_all = 0x20;
inline constexpr std::uint32_t event_mode_mask = event_mode_or | event_mode_clear | event_mode_clear_all;
inline constexpr std::uint32_t event_attr_multi = 0x02;

// Priorities: the boot thread runs at 0; CreateThread and ChangeThreadPriority take 1..127.
inline constexpr std::int32_t priority_count = 128;
// Thread ids: the boot thread is 1, created threads take 2..255.
inline constexpr std::int32_t boot_thread_id = 1;
inline constexpr std::int32_t first_thread_id = 2;
inline constexpr std::int32_t last_thread_id = 255;

// CreateThread's parameter block (36 bytes) and ReferThreadStatus's status block (48 bytes), which extends
// it.
namespace thread_param {
inline constexpr std::uint32_t status = 0x00;
inline constexpr std::uint32_t entry = 0x04;
inline constexpr std::uint32_t stack = 0x08;
inline constexpr std::uint32_t stack_size = 0x0C;
inline constexpr std::uint32_t gp = 0x10;
inline constexpr std::uint32_t initial_priority = 0x14;
inline constexpr std::uint32_t current_priority = 0x18;
inline constexpr std::uint32_t attr = 0x1C;
inline constexpr std::uint32_t option = 0x20;
inline constexpr std::uint32_t size = 0x24;
// Status block only.
inline constexpr std::uint32_t wait_type = 0x24;
inline constexpr std::uint32_t wait_id = 0x28;
inline constexpr std::uint32_t wakeup_count = 0x2C;
inline constexpr std::uint32_t status_size = 0x30;
} // namespace thread_param

// CreateSema's parameter block, also ReferSemaStatus's status block (24 bytes).
namespace sema_param {
inline constexpr std::uint32_t count = 0x00;
inline constexpr std::uint32_t max_count = 0x04;
inline constexpr std::uint32_t init_count = 0x08;
inline constexpr std::uint32_t wait_threads = 0x0C;
inline constexpr std::uint32_t attr = 0x10;
inline constexpr std::uint32_t option = 0x14;
inline constexpr std::uint32_t size = 0x18;
} // namespace sema_param

// CreateEventFlag's parameter block (12 bytes) and ReferEventFlagStatus's status block (28 bytes).
namespace event_flag_param {
inline constexpr std::uint32_t attr = 0x00;
inline constexpr std::uint32_t option = 0x04;
inline constexpr std::uint32_t init_bits = 0x08;
inline constexpr std::uint32_t size = 0x0C;
// Status block.
inline constexpr std::uint32_t current_bits = 0x0C;
inline constexpr std::uint32_t wait_threads = 0x10;
inline constexpr std::uint32_t status_size = 0x1C;
} // namespace event_flag_param

// Timer 3 belongs to the kernel's alarms on the consoles whose kernel implements SetAlarm (EE timer registers
// are public hardware: T3_MODE at physical 0x10001810, bit 8 = CMPE, compare interrupt enable). Programs
// built with Sony's libkernel test T3_MODE.CMPE at start-up and install their own kernel-mode alarm patch
// (syscalls 0xFC-0xFF) only when it is clear. Our alarms are native (event-driven, not on timer 3), so the
// HLE kernel sets CMPE at reset, counting stopped, and the patch is skipped (M3 bring-up,
// Docs/milestones/M3-boots.md).
inline constexpr std::uint32_t timer3_mode = 0x10001810u;
inline constexpr std::uint32_t timer_mode_cmpe = 1u << 8;

// The kernel's syscall table as guest code finds it: SetSyscall writes a handler word at table + 4 * number
// (the number is signed, so SDK kernel patches reach below the table), GetEntryAddress reads one, and the two
// probe words at 0x2F0 and 0x2F8 hold the table address's upper and lower halves the way the SDK's patch code
// reads them out of the kernel's handler instructions. Only writes below `mirror_limit` land in RAM.
inline constexpr std::uint32_t syscall_table_guest = 0x80011F80u;
inline constexpr std::uint32_t syscall_table_physical = syscall_table_guest & 0x1FFFFFFFu;
inline constexpr std::uint32_t syscall_table_probe = 0x000002F0u;
inline constexpr std::uint32_t syscall_mirror_limit = 0x00080000u;

} // namespace dq8::rt::kabi
