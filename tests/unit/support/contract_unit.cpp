// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// The contract unit (see contract_unit.h), written by hand in the emitter's shape. The guest program, all
// ours:
//
//   0x00200000 entry:          lui a0,0x1234; ori a0,a0,0x5678; jal helper; nop;
//   0x00200010                 addiu v1,zero,0x3C; syscall; jr ra; nop
//   0x00200040 unwind_caller:  jal tail_jumper; nop;
//   0x00200048                 addiu v1,zero,0x3D; syscall
//   0x00200060 ends:           addiu v0,zero,7; jr ra; nop
//   0x00200080 FlushCache:     (kernel-call stub, bound by name)
//   0x00200090 calls_flush:    addiu a0,zero,0; jal FlushCache; nop; jr ra; nop
//   0x002000C0 memcpy:         (library stub, bound by name)
//   0x002000E0 calls_missing:  jal 0x002001F0; nop; jr ra; nop
//   0x00200100 helper:         addiu v0,a0,1; jr ra; nop
//   0x00200120 tail_jumper:    lui t9,0x0020; ori t9,t9,0x0140; jr t9; nop
//   0x00200140 jump_target:    addiu s0,zero,0x77; jr ra; nop
//   0x00200160 scratchpad:     lui t0,0x7000; addiu t1,zero,0x5A5A; sw t1,0x10(t0); lw t2,0x10(t0); jr ra;
//   nop 0x002001A0 unhandled:      (an instruction the recompiler left as a throw) 0x002001C0 recurse: jal
//   recurse; nop
//
// The M3 programs (contract_unit.h lists what each does) are written as the emitter writes them too: a
// syscall is `addiu v1,zero,N; syscall` and continues inline, a back-edge is `ctx->pc = target;
// if (runtime->eeCheckpointDue()) return; goto label;`, a call saves nothing but $ra in $s2.
#include "support/contract_unit.h"

#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"

#include <stdexcept>

void FlushCache_0x200080(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
void memcpy_0x2000c0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);

// Function: entry
// Address: 0x200000 - 0x200020
void entry_0x200000(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    switch (ctx->pc) {
    case 0x200010u:
        goto label_200010;
    default:
        break;
    }
    ctx->pc = 0x200000u;

    ctx->pc = 0x200000u;
    SET_GPR_U32(ctx, 4, (0x1234u << 16));
    ctx->pc = 0x200004u;
    SET_GPR_U32(ctx, 4, GPR_U32(ctx, 4) | 0x5678u);
    SET_GPR_U32(ctx, 31, 0x200010u);
    ctx->pc = 0x200100u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x200100u, 0x200008u, 0x200010u,
                                      PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_200010:
    ctx->pc = 0x200010u;
    SET_GPR_U32(ctx, 3, 0x3Cu);
    ctx->pc = 0x200014u;
    ctx->pc = 0x200018u;
    runtime->handleSyscall(rdram, ctx, 0x0u);
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        ctx->pc = jumpTarget;
        return;
    }
}

// Function: unwind_caller
// Address: 0x200040 - 0x200050
void unwind_caller_0x200040(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    switch (ctx->pc) {
    case 0x200048u:
        goto label_200048;
    default:
        break;
    }
    ctx->pc = 0x200040u;

    SET_GPR_U32(ctx, 31, 0x200048u);
    ctx->pc = 0x200120u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x200120u, 0x200040u, 0x200048u,
                                      PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_200048:
    ctx->pc = 0x200048u;
    SET_GPR_U32(ctx, 3, 0x3Du);
    ctx->pc = 0x20004cu;
    ctx->pc = 0x200050u;
    runtime->handleSyscall(rdram, ctx, 0x0u);
    ctx->pc = 0x200050u;
}

// Function: ends
// Address: 0x200060 - 0x20006c
void ends_0x200060(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    (void)rdram;
    (void)runtime;
    ctx->pc = 0x200060u;

    ctx->pc = 0x200060u;
    SET_GPR_U32(ctx, 2, 7u);
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        ctx->pc = jumpTarget;
        return;
    }
}

// Stub: a kernel-call wrapper bound by name (Name@0xADDR in the recompiler's configuration).
void FlushCache_0x200080(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = getRegU32(ctx, 31);
    ps2_syscalls::FlushCache(rdram, ctx, runtime);
}

// Function: calls_flush
// Address: 0x200090 - 0x2000a4
void calls_flush_0x200090(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    switch (ctx->pc) {
    case 0x20009cu:
        goto label_20009c;
    default:
        break;
    }
    ctx->pc = 0x200090u;

    ctx->pc = 0x200090u;
    SET_GPR_U32(ctx, 4, 0u);
    SET_GPR_U32(ctx, 31, 0x20009cu);
    ctx->pc = 0x200080u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x200080u, 0x200094u, 0x20009cu,
                                      PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_20009c: {
    const uint32_t jumpTarget = GPR_U32(ctx, 31);
    ctx->pc = jumpTarget;
    ctx->pc = jumpTarget;
    return;
}
}

// Stub: a library function bound by name.
void memcpy_0x2000c0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = getRegU32(ctx, 31);
    ps2_stubs::memcpy(rdram, ctx, runtime);
}

// Function: calls_missing
// Address: 0x2000e0 - 0x2000f0
void calls_missing_0x2000e0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    switch (ctx->pc) {
    case 0x2000e8u:
        goto label_2000e8;
    default:
        break;
    }
    ctx->pc = 0x2000e0u;

    SET_GPR_U32(ctx, 31, 0x2000e8u);
    ctx->pc = 0x2001f0u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x2001f0u, 0x2000e0u, 0x2000e8u,
                                      PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_2000e8: {
    const uint32_t jumpTarget = GPR_U32(ctx, 31);
    ctx->pc = jumpTarget;
    ctx->pc = jumpTarget;
    return;
}
}

// Function: helper
// Address: 0x200100 - 0x20010c
void helper_0x200100(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    (void)rdram;
    (void)runtime;
    ctx->pc = 0x200100u;

    ctx->pc = 0x200100u;
    SET_GPR_U32(ctx, 2, GPR_U32(ctx, 4) + 1u);
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        ctx->pc = jumpTarget;
        return;
    }
}

// Function: tail_jumper
// Address: 0x200120 - 0x200130
void tail_jumper_0x200120(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = 0x200120u;

    ctx->pc = 0x200120u;
    SET_GPR_U32(ctx, 25, (0x0020u << 16));
    ctx->pc = 0x200124u;
    SET_GPR_U32(ctx, 25, GPR_U32(ctx, 25) | 0x0140u);
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 25);
        ctx->pc = jumpTarget;
        if (!runtime->dispatchGuestBranch(rdram, ctx, jumpTarget, 0x200128u, 0u,
                                          PS2Runtime::GuestBranchKind::IndirectJump, "JR")) {
            return;
        }
    }
}

// Function: jump_target
// Address: 0x200140 - 0x20014c
void jump_target_0x200140(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    (void)rdram;
    (void)runtime;
    ctx->pc = 0x200140u;

    ctx->pc = 0x200140u;
    SET_GPR_U32(ctx, 16, 0x77u);
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        ctx->pc = jumpTarget;
        return;
    }
}

// Function: scratchpad
// Address: 0x200160 - 0x200178
void scratchpad_0x200160(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = 0x200160u;

    ctx->pc = 0x200160u;
    SET_GPR_U32(ctx, 8, (0x7000u << 16));
    ctx->pc = 0x200164u;
    SET_GPR_U32(ctx, 9, 0x5A5Au);
    ctx->pc = 0x200168u;
    FAST_WRITE32(0x70000010u, GPR_U32(ctx, 9)); // constant-address hint: the emitter writes the literal
    ctx->pc = 0x20016cu;
    SET_GPR_S32(ctx, 10, static_cast<int32_t>(READ32(ADD32(GPR_U32(ctx, 8), 0x10u))));
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        ctx->pc = jumpTarget;
        return;
    }
}

// Function: unhandled
// Address: 0x2001a0 - 0x2001a8
void unhandled_0x2001a0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    (void)rdram;
    (void)runtime;
    ctx->pc = 0x2001a0u;

    ctx->pc = 0x2001a0u;
    throw std::runtime_error("Unhandled opcode at 0x2001a0");
}

// Function: recurse
// Address: 0x2001c0 - 0x2001c8
void recurse_0x2001c0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    switch (ctx->pc) {
    case 0x2001c8u:
        goto label_2001c8;
    default:
        break;
    }
    ctx->pc = 0x2001c0u;

    SET_GPR_U32(ctx, 31, 0x2001c8u);
    ctx->pc = 0x2001c0u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x2001c0u, 0x2001c0u, 0x2001c8u,
                                      PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_2001c8:
    ctx->pc = 0x2001c8u;
}

// ---- M3 programs.

namespace contract {
Counters& counters() {
    static Counters c;
    return c;
}
} // namespace contract

// Function: two_threads
// Address: 0x200200 - 0x200258
void two_threads_0x200200(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = 0x200200u;
    // CreateSema: max_count 1, init_count 0 in the parameter block at data.
    WRITE32(contract::data + 0x04u, 1u);
    WRITE32(contract::data + 0x08u, 0u);
    SET_GPR_U32(ctx, 4, contract::data);
    SET_GPR_U32(ctx, 3, 0x40u);
    ctx->pc = 0x200214u;
    runtime->handleSyscall(rdram, ctx, 0x0u);
    WRITE32(contract::data + 0x80u, GPR_U32(ctx, 2));
    // CreateThread: entry, stack, stack size and priority 10 in the parameter block at data+0x20.
    WRITE32(contract::data + 0x24u, contract::worker);
    WRITE32(contract::data + 0x28u, contract::worker_stack);
    WRITE32(contract::data + 0x2Cu, 0x4000u);
    WRITE32(contract::data + 0x34u, 10u);
    SET_GPR_U32(ctx, 4, contract::data + 0x20u);
    SET_GPR_U32(ctx, 3, 0x20u);
    ctx->pc = 0x200234u;
    runtime->handleSyscall(rdram, ctx, 0x0u);
    // StartThread(thread, semaphore)
    SET_GPR_U32(ctx, 4, GPR_U32(ctx, 2));
    SET_GPR_U32(ctx, 5, READ32(contract::data + 0x80u));
    SET_GPR_U32(ctx, 3, 0x22u);
    ctx->pc = 0x200244u;
    runtime->handleSyscall(rdram, ctx, 0x0u);
    // WaitSema(semaphore): blocks until the worker signals
    SET_GPR_U32(ctx, 4, READ32(contract::data + 0x80u));
    SET_GPR_U32(ctx, 3, 0x44u);
    ctx->pc = 0x200250u;
    runtime->handleSyscall(rdram, ctx, 0x0u);
    SET_GPR_U32(ctx, 16, GPR_U32(ctx, 2));
    SET_GPR_U32(ctx, 17, READ32(contract::data + 0x84u));
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        return;
    }
}

// Function: worker
// Address: 0x200260 - 0x200278
void worker_0x200260(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = 0x200260u;
    WRITE32(contract::data + 0x84u, 0xB0Bu);
    SET_GPR_U32(ctx, 3, 0x42u); // SignalSema($a0)
    ctx->pc = 0x200270u;
    runtime->handleSyscall(rdram, ctx, 0x0u);
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        return;
    }
}

// Function: vblank_sema
// Address: 0x2002a0 - 0x2002f8
void vblank_sema_0x2002a0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = 0x2002A0u;
    WRITE32(contract::data + 0x04u, 1u);
    WRITE32(contract::data + 0x08u, 0u);
    SET_GPR_U32(ctx, 4, contract::data);
    SET_GPR_U32(ctx, 3, 0x40u); // CreateSema
    ctx->pc = 0x2002B4u;
    runtime->handleSyscall(rdram, ctx, 0x0u);
    WRITE32(contract::data + 0x80u, GPR_U32(ctx, 2));
    SET_GPR_U32(ctx, 7, GPR_U32(ctx, 2));
    SET_GPR_U32(ctx, 4, 2u); // INTC 2: vblank start
    SET_GPR_U32(ctx, 5, contract::vblank_handler);
    SET_GPR_U32(ctx, 6, 0u);
    SET_GPR_U32(ctx, 3, 0x10u); // AddIntcHandler(2, handler, 0, semaphore)
    ctx->pc = 0x2002CCu;
    runtime->handleSyscall(rdram, ctx, 0x0u);
    SET_GPR_U32(ctx, 4, 2u);
    SET_GPR_U32(ctx, 3, 0x14u); // EnableIntc(2)
    ctx->pc = 0x2002D8u;
    runtime->handleSyscall(rdram, ctx, 0x0u);
    SET_GPR_U32(ctx, 4, READ32(contract::data + 0x80u));
    SET_GPR_U32(ctx, 3, 0x44u); // WaitSema
    ctx->pc = 0x2002E4u;
    runtime->handleSyscall(rdram, ctx, 0x0u);
    SET_GPR_U32(ctx, 16, GPR_U32(ctx, 2));
    SET_GPR_U32(ctx, 17, READ32(contract::data + 0x88u));
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        return;
    }
}

// Function: vblank_handler
// Address: 0x200300 - 0x200318
void vblank_handler_0x200300(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = 0x200300u;
    WRITE32(contract::data + 0x88u, ADD32(READ32(contract::data + 0x88u), 1u));
    SET_GPR_U32(ctx, 4, GPR_U32(ctx, 5));
    SET_GPR_U32(ctx, 3, static_cast<uint32_t>(-0x43)); // iSignalSema($a1)
    ctx->pc = 0x200310u;
    runtime->handleSyscall(rdram, ctx, 0x0u);
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        return;
    }
}

// Function: backedge_irq
// Address: 0x200320 - 0x200368
void backedge_irq_0x200320(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    switch (ctx->pc) {
    case 0x200358u:
        goto label_200358;
    default:
        break;
    }
    ctx->pc = 0x200320u;
    SET_GPR_U32(ctx, 18, GPR_U32(ctx, 31));
    SET_GPR_U32(ctx, 4, 2u);
    SET_GPR_U32(ctx, 5, contract::flag_handler);
    SET_GPR_U32(ctx, 6, 0u);
    SET_GPR_U32(ctx, 7, 0u);
    SET_GPR_U32(ctx, 3, 0x10u); // AddIntcHandler(2, flag_handler, 0, 0)
    ctx->pc = 0x20033Cu;
    runtime->handleSyscall(rdram, ctx, 0x0u);
    SET_GPR_U32(ctx, 4, 2u);
    SET_GPR_U32(ctx, 3, 0x14u); // EnableIntc(2)
    ctx->pc = 0x200348u;
    runtime->handleSyscall(rdram, ctx, 0x0u);
    SET_GPR_U32(ctx, 31, 0x200358u);
    ctx->pc = 0x200380u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x200380u, 0x200350u, 0x200358u,
                                      PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_200358:
    ctx->pc = 0x200358u;
    SET_GPR_U32(ctx, 17, READ32(contract::data + 0x90u));
    SET_GPR_U32(ctx, 31, GPR_U32(ctx, 18));
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        return;
    }
}

// Function: spin_loop
// Address: 0x200380 - 0x200398
void spin_loop_0x200380(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ++contract::counters().spin_loop_entries;
    switch (ctx->pc) {
    case 0x200384u:
        goto label_200384;
    default:
        break;
    }
    ctx->pc = 0x200380u;
label_200384:
    ctx->pc = 0x200384u;
    SET_GPR_U32(ctx, 8, READ32(contract::data + 0x90u));
    if (GPR_U32(ctx, 8) == 0u) {
        ctx->pc = 0x200384u;
        if (runtime->eeCheckpointDue()) {
            return;
        }
        goto label_200384;
    }
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        return;
    }
}

// Function: flag_handler
// Address: 0x2003c0 - 0x2003cc
void flag_handler_0x2003c0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = 0x2003C0u;
    WRITE32(contract::data + 0x90u, 1u);
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        return;
    }
}

// Function: self_loop_caller
// Address: 0x2003e0 - 0x200420
void self_loop_caller_0x2003e0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    switch (ctx->pc) {
    case 0x200410u:
        goto label_200410;
    default:
        break;
    }
    ctx->pc = 0x2003E0u;
    SET_GPR_U32(ctx, 18, GPR_U32(ctx, 31));
    SET_GPR_U32(ctx, 4, 9u); // INTC 9: timer 0
    SET_GPR_U32(ctx, 5, contract::count_handler);
    SET_GPR_U32(ctx, 6, 0u);
    SET_GPR_U32(ctx, 7, 0u);
    SET_GPR_U32(ctx, 3, 0x10u); // AddIntcHandler(9, count_handler, 0, 0)
    ctx->pc = 0x2003FCu;
    runtime->handleSyscall(rdram, ctx, 0x0u);
    SET_GPR_U32(ctx, 4, 9u);
    SET_GPR_U32(ctx, 3, 0x14u); // EnableIntc(9)
    ctx->pc = 0x200404u;
    runtime->handleSyscall(rdram, ctx, 0x0u);
    SET_GPR_U32(ctx, 16, 0u);
    SET_GPR_U32(ctx, 31, 0x200410u);
    ctx->pc = 0x200420u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x200420u, 0x200408u, 0x200410u,
                                      PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_200410:
    ctx->pc = 0x200410u;
    SET_GPR_U32(ctx, 17, 0x5E1Fu);
    SET_GPR_U32(ctx, 31, GPR_U32(ctx, 18));
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        return;
    }
}

// Function: self_loop (its loop head is its own entry)
// Address: 0x200420 - 0x200438
void self_loop_0x200420(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    (void)rdram;
    ++contract::counters().self_loop_entries;
    ctx->pc = 0x200420u;
label_200420:
    ctx->pc = 0x200420u;
    SET_GPR_U32(ctx, 16, ADD32(GPR_U32(ctx, 16), 1u));
    ctx->pc = 0x200424u;
    if (GPR_U32(ctx, 16) < contract::self_loop_iterations) {
        ctx->pc = 0x200420u;
        if (runtime->eeCheckpointDue()) {
            return;
        }
        goto label_200420;
    }
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        return;
    }
}

// Function: jr_self_caller
// Address: 0x200460 - 0x200480
void jr_self_caller_0x200460(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    switch (ctx->pc) {
    case 0x200470u:
        goto label_200470;
    default:
        break;
    }
    ctx->pc = 0x200460u;
    SET_GPR_U32(ctx, 18, GPR_U32(ctx, 31));
    SET_GPR_U32(ctx, 16, 0u);
    SET_GPR_U32(ctx, 31, 0x200470u);
    ctx->pc = 0x2004A0u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x2004A0u, 0x200468u, 0x200470u,
                                      PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_200470:
    ctx->pc = 0x200470u;
    SET_GPR_U32(ctx, 17, 0x15E1u);
    SET_GPR_U32(ctx, 31, GPR_U32(ctx, 18));
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        return;
    }
}

// Function: jr_self (re-enters itself with `jr $t9` while $s0 < 3)
// Address: 0x2004a0 - 0x2004c4
void jr_self_0x2004a0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ++contract::counters().jr_self_entries;
    ctx->pc = 0x2004A0u;
    SET_GPR_U32(ctx, 16, ADD32(GPR_U32(ctx, 16), 1u));
    ctx->pc = 0x2004A4u;
    if (GPR_U32(ctx, 16) < 3u) {
        SET_GPR_U32(ctx, 25, 0x2004A0u);
        const uint32_t jumpTarget = GPR_U32(ctx, 25);
        ctx->pc = jumpTarget;
        if (!runtime->dispatchGuestBranch(rdram, ctx, jumpTarget, 0x2004B0u, 0u,
                                          PS2Runtime::GuestBranchKind::IndirectJump, "JR")) {
            return;
        }
    }
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        return;
    }
}

// Function: count_handler
// Address: 0x2004e0 - 0x2004ec
void count_handler_0x2004e0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = 0x2004E0u;
    WRITE32(contract::data + 0x94u, ADD32(READ32(contract::data + 0x94u), 1u));
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        return;
    }
}

// The unit's dense table, as register_functions.cpp declares it (renamed per unit by the module build).
extern const uint32_t dq8_unit_contract_table_base = contract::table_base;
extern const uint32_t dq8_unit_contract_table_end = contract::table_end;
extern const uint32_t dq8_unit_contract_table_slot_count = (contract::table_end - contract::table_base) / 4u;
PS2Runtime::RecompiledFunction dq8_unit_contract_table[(contract::table_end - contract::table_base) / 4u] =
    {};

namespace {
struct GeneratedFunctionTableInitializer {
    GeneratedFunctionTableInitializer() {
        dq8_unit_contract_table[0] = entry_0x200000;              // 0x200000
        dq8_unit_contract_table[4] = entry_0x200000;              // 0x200010 (resume)
        dq8_unit_contract_table[16] = unwind_caller_0x200040;     // 0x200040
        dq8_unit_contract_table[18] = unwind_caller_0x200040;     // 0x200048 (resume)
        dq8_unit_contract_table[24] = ends_0x200060;              // 0x200060
        dq8_unit_contract_table[32] = FlushCache_0x200080;        // 0x200080
        dq8_unit_contract_table[36] = calls_flush_0x200090;       // 0x200090
        dq8_unit_contract_table[39] = calls_flush_0x200090;       // 0x20009c (resume)
        dq8_unit_contract_table[48] = memcpy_0x2000c0;            // 0x2000c0
        dq8_unit_contract_table[56] = calls_missing_0x2000e0;     // 0x2000e0
        dq8_unit_contract_table[58] = calls_missing_0x2000e0;     // 0x2000e8 (resume)
        dq8_unit_contract_table[64] = helper_0x200100;            // 0x200100
        dq8_unit_contract_table[72] = tail_jumper_0x200120;       // 0x200120
        dq8_unit_contract_table[80] = jump_target_0x200140;       // 0x200140
        dq8_unit_contract_table[88] = scratchpad_0x200160;        // 0x200160
        dq8_unit_contract_table[104] = unhandled_0x2001a0;        // 0x2001a0
        dq8_unit_contract_table[112] = recurse_0x2001c0;          // 0x2001c0
        dq8_unit_contract_table[114] = recurse_0x2001c0;          // 0x2001c8 (resume)
        dq8_unit_contract_table[128] = two_threads_0x200200;      // 0x200200
        dq8_unit_contract_table[152] = worker_0x200260;           // 0x200260
        dq8_unit_contract_table[168] = vblank_sema_0x2002a0;      // 0x2002a0
        dq8_unit_contract_table[192] = vblank_handler_0x200300;   // 0x200300
        dq8_unit_contract_table[200] = backedge_irq_0x200320;     // 0x200320
        dq8_unit_contract_table[214] = backedge_irq_0x200320;     // 0x200358 (resume)
        dq8_unit_contract_table[224] = spin_loop_0x200380;        // 0x200380
        dq8_unit_contract_table[225] = spin_loop_0x200380;        // 0x200384 (back-edge target)
        dq8_unit_contract_table[240] = flag_handler_0x2003c0;     // 0x2003c0
        dq8_unit_contract_table[248] = self_loop_caller_0x2003e0; // 0x2003e0
        dq8_unit_contract_table[260] = self_loop_caller_0x2003e0; // 0x200410 (resume)
        dq8_unit_contract_table[264] = self_loop_0x200420;        // 0x200420
        dq8_unit_contract_table[280] = jr_self_caller_0x200460;   // 0x200460
        dq8_unit_contract_table[284] = jr_self_caller_0x200460;   // 0x200470 (resume)
        dq8_unit_contract_table[296] = jr_self_0x2004a0;          // 0x2004a0
        dq8_unit_contract_table[312] = count_handler_0x2004e0;    // 0x2004e0
    }
};
const GeneratedFunctionTableInitializer g_generatedFunctionTableInitializer;
} // namespace

const dq8_game_module_desc* contract_unit_descriptor() {
    static const dq8_game_module_desc desc{
        DQ8_GAME_MODULE_MAGIC,
        DQ8_GAME_MODULE_ABI_VERSION,
        sizeof(dq8_game_module_desc),
        DQ8_UNIT_MAIN,
        0,
        contract::unit_name,
        "SYNTHETIC",
        contract::source_sha1,
        "hand-written contract unit",
        sizeof(R5900Context),
        alignof(R5900Context),
        contract::entry,
        dq8_unit_contract_table_base,
        dq8_unit_contract_table_end,
        dq8_unit_contract_table_slot_count,
        dq8_unit_contract_table,
    };
    return &desc;
}
