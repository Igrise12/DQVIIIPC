// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// ps2_syscalls::<name> for every name in PS2_SYSCALL_LIST (ps2_call_list.h). Generated code calls these from
// the body of a function the recompiler config binds to a kernel call ("Name@0xADDR" in `stubs`). Each call
// goes to the host by name, as DQ8_CALL_SYSCALL (ps2_named_call.h says how); the host decides what the name
// means. Upstream's header declares the same names and links them against its own kernel; ours are
// header-only, so a game module has no undefined references (game_module.h).
#pragma once

#include "ps2_call_list.h"
#include "ps2_named_call.h"
#include "ps2_runtime.h"

namespace ps2_syscalls {

#define DQ8_PS2_SYSCALL_FORWARDER(name) using name = dq8_named_call<DQ8_CALL_SYSCALL, #name>;
PS2_SYSCALL_LIST(DQ8_PS2_SYSCALL_FORWARDER)
#undef DQ8_PS2_SYSCALL_FORWARDER

} // namespace ps2_syscalls
