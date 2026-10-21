// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>

struct R5900Context;

// VU0 micro mode started from EE code (architecture §5.4; M4 day 0): VCALLMS and VCALLMSR run a VU0 micro
// program to its end on the calling thread's VU0 registers, which the recompiled code keeps in its
// R5900Context (macro mode). The VU device implements this and registers itself with the machine
// (MachineServices::set_vu0); EeRuntime calls it. VIF0's MSCAL runs programs on the unit's own register file
// until M4 session D shares the two views.
namespace dq8::rt {

class IVu0Runner {
  public:
    virtual ~IVu0Runner() = default;
    // Runs the program at byte address `start` (a multiple of 8 within VU0 micro memory) with `ctx`'s VU0
    // registers in and out. Returns the VU0 cycles it took (VU0 runs at the EE clock); the caller charges
    // them. A reserved instruction or a program that does not end stops the machine.
    virtual std::uint64_t run_from_ee(R5900Context& ctx, std::uint32_t start) = 0;
};

} // namespace dq8::rt
