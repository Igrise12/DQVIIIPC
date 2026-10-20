// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Hand-written in the shape of the recompiler's output for a function with an instruction it cannot translate.
#include <stdexcept>
#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"
#include <ps2_recompiled_functions.h>
#include <ps2_recompiled_stubs.h>

#include "ps2_syscalls.h"
#include "ps2_stubs.h"

#ifdef PS2_FUNCTION_LOG_TRACKER
#include "ps2_log.h"
#endif

// Function: sample_unhandled
// Address: 0x2000e0 - 0x2000e8
void sample_unhandled_0x2000e0(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
#ifdef PS2_FUNCTION_LOG_TRACKER
    PS_LOG_ENTRY("sample_unhandled_0x2000e0");
#endif

    ctx->pc = 0x2000e0u;

    // 0x2000e0: (an instruction the recompiler has no translation for)
    ctx->pc = 0x2000e0u;
    throw std::runtime_error("Unhandled instruction at 0x2000E0 raw=0xFFFFFFFF");
}
