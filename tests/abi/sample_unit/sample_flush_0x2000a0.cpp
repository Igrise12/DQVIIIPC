// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Hand-written in the shape of the recompiler's output for a function bound to a kernel call
// ("FlushCache@0x2000a0" in the config's stubs).
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#ifdef _DEBUG
#include "ps2_log.h"
#endif

void sample_flush_0x2000a0(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
#ifdef _DEBUG
    PS_LOG_ENTRY("sample_flush_0x2000a0");
#endif
    ctx->pc = getRegU32(ctx, 31);
    ps2_syscalls::FlushCache(rdram, ctx, runtime);
}
