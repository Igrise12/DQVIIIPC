// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Hand-written in the shape of the recompiler's output for a function whose map name is in PS2_STUB_LIST.
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#ifdef _DEBUG
#include "ps2_log.h"
#endif

void puts_0x200080(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
#ifdef _DEBUG
    PS_LOG_ENTRY("puts_0x200080");
#endif
    ctx->pc = getRegU32(ctx, 31);
    ps2_stubs::puts(rdram, ctx, runtime);
}
