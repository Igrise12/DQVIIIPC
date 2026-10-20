// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Hand-written in the shape of the recompiler's output for one function. A synthetic unit, not game code: the
// guest instructions in the comments were made up for the ABI test (tests/abi/README.md lists what it checks).
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

// Function: sample_start
// Address: 0x200000 - 0x20004c
void sample_start_0x200000(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
#ifdef PS2_FUNCTION_LOG_TRACKER
    PS_LOG_ENTRY("sample_start_0x200000");
#endif

    switch (ctx->pc) {
        case 0x200010u: goto label_200010;
        case 0x20002cu: goto label_20002c;
        case 0x200034u: goto label_200034;
        case 0x20003cu: goto label_20003c;
        case 0x200044u: goto label_200044;
        default: break;
    }

    ctx->pc = 0x200000u;

    // 0x200000: lui $a0, 0x1234
    ctx->pc = 0x200000u;
    SET_GPR_S32(ctx, 4, (int32_t)(0x1234u << 16));
    // 0x200004: ori $a0, $a0, 0x5678
    ctx->pc = 0x200004u;
    SET_GPR_U64(ctx, 4, GPR_U64(ctx, 4) | 0x5678u);
    // 0x200008: jal 0x200060
    ctx->pc = 0x200008u;
    SET_GPR_U32(ctx, 31, 0x200010u);
    // 0x20000c: nop
    ctx->pc = 0x20000cu;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x200060u, 0x200008u, 0x200010u, PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_200010:
    // 0x200010: lui $t0, 0x7000
    ctx->pc = 0x200010u;
    SET_GPR_S32(ctx, 8, (int32_t)(0x7000u << 16));
    // 0x200014: sw $v0, 0x10($t0)  (scratchpad)
    ctx->pc = 0x200014u;
    WRITE32(ADD32(GPR_U32(ctx, 8), 16), GPR_U32(ctx, 2));
    // 0x200018: lui $t1, 0x1100
    ctx->pc = 0x200018u;
    SET_GPR_S32(ctx, 9, (int32_t)(0x1100u << 16));
    // 0x20001c: sw $a0, 0x10($t1)  (constant address in VU0 micro memory, which upstream's predicate calls RAM)
    ctx->pc = 0x20001cu;
    do { uint32_t _value = static_cast<uint32_t>(GPR_U32(ctx, 4)); ps2TraceGuestWrite(rdram, 0x11000010u, 4u, _value, 0u, "WRITE32", ctx); FAST_WRITE32(0x11000010u, _value); } while (0);
    // 0x200020: lw $a1, 0x0($t2)  (constant address 0x300000 in RAM)
    ctx->pc = 0x200020u;
    SET_GPR_S32(ctx, 5, (int32_t)FAST_READ32(0x300000u));
    // 0x200024: jal 0x200080
    ctx->pc = 0x200024u;
    SET_GPR_U32(ctx, 31, 0x20002cu);
    // 0x200028: nop
    ctx->pc = 0x200028u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x200080u, 0x200024u, 0x20002Cu, PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_20002c:
    // 0x20002c: jal 0x2000a0
    ctx->pc = 0x20002cu;
    SET_GPR_U32(ctx, 31, 0x200034u);
    // 0x200030: nop
    ctx->pc = 0x200030u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x2000A0u, 0x20002Cu, 0x200034u, PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_200034:
    // 0x200034: jal 0x2000c0
    ctx->pc = 0x200034u;
    SET_GPR_U32(ctx, 31, 0x20003cu);
    // 0x200038: nop
    ctx->pc = 0x200038u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x2000C0u, 0x200034u, 0x20003Cu, PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_20003c:
    // 0x20003c: addiu $v1, $zero, 0x3c
    ctx->pc = 0x20003cu;
    SET_GPR_S32(ctx, 3, ADD32(GPR_U32(ctx, 0), 0x3c));
    // 0x200040: syscall
    ctx->pc = 0x200040u;
    ctx->pc = 0x200044u;
    runtime->handleSyscall(rdram, ctx, 0x0u);
label_200044:
    // 0x200044: jr $ra
    ctx->pc = 0x200044u;
    // 0x200048: nop
    ctx->pc = 0x200048u;
#if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS
    (void)runtime->dispatchGuestBranch(rdram, ctx, GPR_U32(ctx, 31), 0x200044u, 0u, PS2Runtime::GuestBranchKind::Return, "JR $ra");
    return;
#else
    ctx->pc = GPR_U32(ctx, 31);
    return;
#endif
}
