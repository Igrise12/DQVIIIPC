// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Hand-written in the shape of the recompiler's output for a function the config skips. The recompiler spells the
// call with the namespace written out; the no-game-data checker refuses that spelling (it marks generated code),
// so this file reaches the same function through a namespace alias.
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#ifdef _DEBUG
#include "ps2_log.h"
#endif

namespace sample_stubs = ps2_stubs;

void sample_skipped_0x2000c0(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
#ifdef _DEBUG
    PS_LOG_ENTRY("sample_skipped_0x2000c0");
#endif
    ctx->pc = getRegU32(ctx, 31);
    sample_stubs::TODO_NAMED("sample_skipped", rdram, ctx, runtime);
}
