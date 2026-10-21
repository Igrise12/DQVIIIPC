// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/vu/vu_interpreter.h"

struct R5900Context;

// The VU0 register file as the recompiled code keeps it (R5900Context, macro mode) and as the VU0 interpreter
// keeps it (VuState): the copies around a micro program started from EE code (vu0_runner.h).
namespace dq8::rt {

// Copies the VU0 registers of `ctx` into `state` and clears the interpreter's run state (pc, branch, stop
// flags). VF0 and VI0 get their fixed values. The cycle count is kept: it runs on across programs, and
// callers take differences.
void load_vu0_state(const R5900Context& ctx, VuState& state);
// Copies the registers back after the program, with the program's end address in vu0_tpc and the D/T stop
// bits in VPU_STAT.
void store_vu0_state(const VuState& state, R5900Context& ctx);

} // namespace dq8::rt
