// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>

// The EE kernel's native services owned by the kernel itself (Docs/milestones/M3-boots.md, "Kernel calls",
// owner A): threads, semaphores, event flags, alarms, interrupt handlers and the system calls. The Machine
// registers them in its constructor, with owner "kernel"; the GS and SIF calls come from their devices.
namespace dq8::rt {

class KernelServices;
class EeKernel;
class GuestMemory;
struct KernelSystemState;

void register_thread_services(KernelServices& services, EeKernel& kernel);
void register_sync_services(KernelServices& services, EeKernel& kernel);
void register_interrupt_services(KernelServices& services, EeKernel& kernel);
void register_system_services(KernelServices& services, EeKernel& kernel);

inline void register_ee_kernel_services(KernelServices& services, EeKernel& kernel) {
    register_thread_services(services, kernel);
    register_sync_services(services, kernel);
    register_interrupt_services(services, kernel);
    register_system_services(services, kernel);
}

// The power-on system state: no heap yet, and the OSD configuration (English, 4:3, RGB, no timezone offset).
void reset_system_state(KernelSystemState& state) noexcept;

namespace detail {

// Whether [address, address + size) is guest RAM (any mirror) or scratchpad and `address` is aligned to
// `align`: what the kernel accepts for a parameter or status block.
bool guest_block_ok(std::uint32_t address, std::uint32_t size, std::uint32_t align = 4) noexcept;
std::uint32_t read_u32(GuestMemory& memory, std::uint32_t address);
void write_u32(GuestMemory& memory, std::uint32_t address, std::uint32_t value);

} // namespace detail

} // namespace dq8::rt
