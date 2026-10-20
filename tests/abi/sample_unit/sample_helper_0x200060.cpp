// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Hand-written in the shape of the recompiler's output for one function. A synthetic unit, not game code.
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

// Function: sample_helper
// Address: 0x200060 - 0x200080
void sample_helper_0x200060(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
#ifdef PS2_FUNCTION_LOG_TRACKER
    PS_LOG_ENTRY("sample_helper_0x200060");
#endif

    switch (ctx->pc) {
        case 0x200068u: goto label_200068;
        default: break;
    }

    ctx->pc = 0x200060u;

    // 0x200060: addiu $v0, $a0, -1
    ctx->pc = 0x200060u;
    SET_GPR_S32(ctx, 2, ADD32(GPR_U32(ctx, 4), -1));
    // 0x200064: addiu $t0, $zero, 2
    ctx->pc = 0x200064u;
    SET_GPR_S32(ctx, 8, ADD32(GPR_U32(ctx, 0), 2));
label_200068:
    // 0x200068: addiu $v0, $v0, 1
    ctx->pc = 0x200068u;
    SET_GPR_S32(ctx, 2, ADD32(GPR_U32(ctx, 2), 1));
    // 0x20006c: addiu $t0, $t0, -1
    ctx->pc = 0x20006cu;
    SET_GPR_S32(ctx, 8, ADD32(GPR_U32(ctx, 8), -1));
    // 0x200070: bgtz $t0, 0x200068
    ctx->pc = 0x200070u;
    {
        const bool branch_taken = GPR_S64(ctx, 8) > 0;
        // 0x200074: nop
        ctx->pc = 0x200074u;
        if (branch_taken) {
            ctx->pc = 0x200068u;
            if (runtime->eeCheckpointDue()) {
                return;
            }
            goto label_200068;
        }
    }
    // 0x200078: jr $ra
    ctx->pc = 0x200078u;
    // 0x20007c: nop
    ctx->pc = 0x20007cu;
#if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS
    (void)runtime->dispatchGuestBranch(rdram, ctx, GPR_U32(ctx, 31), 0x200078u, 0u, PS2Runtime::GuestBranchKind::Return, "JR $ra");
    return;
#else
    ctx->pc = GPR_U32(ctx, 31);
    return;
#endif
}
