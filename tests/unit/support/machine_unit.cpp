// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// The machine unit (machine_unit.h), written by hand in the emitter's shape. The guest program, all ours
// (one line per guest function; syscall numbers in v1):
//
//   0x00200000 start:       SetupThread(0, main_stack, main_stack_size, 0, 0); sp = v0; jal main; Exit
//   0x00200100 main:        jal gs_init; vsync_sema = CreateSema(&sema_param); work_sema = CreateSema(...);
//                           handler = AddIntcHandler(2, vblank_handler, 0); EnableIntc(2);
//                           worker = CreateThread(&thread_param); StartThread(worker, 0); jal read_file; EI;
//                           loop: WaitSema(vsync_sema); jal draw_frame; SignalSema(work_sema); b loop
//   0x00200300 gs_init:     GsSetCrt(1, 2, 1); PMODE, DISPFB1, DISPLAY1 (sd); D_CTRL = 1; jr ra
//   0x00200400 vblank_handler: vblanks += 1; iSignalSema(vsync_sema); v0 = 0; jr ra
//   0x00200500 draw_frame:  rgbaq.r = frames * 8; D2_MADR = gif_packet; D2_QWC = 9; D2_CHCR = 0x101;
//                           wait: if (D2_CHCR & 0x100) b wait; frames += 1; jr ra
//   0x00200580 worker:      loop: WaitSema(work_sema); work += 1; b loop
//   0x00200600 read_file:   the fileio read (see read_file below); jr ra
#include "support/machine_unit.h"

#include "ps2_runtime_macros.h"

namespace {

using namespace machine_unit;

constexpr uint32_t sys_exit = 0x04;
constexpr uint32_t sys_gs_set_crt = 0x02;
constexpr uint32_t sys_add_intc_handler = 0x10;
constexpr uint32_t sys_enable_intc = 0x14;
constexpr uint32_t sys_create_thread = 0x20;
constexpr uint32_t sys_start_thread = 0x22;
constexpr uint32_t sys_setup_thread = 0x3C;
constexpr uint32_t sys_create_sema = 0x40;
constexpr uint32_t sys_signal_sema = 0x42;
constexpr uint32_t sys_isignal_sema = 0xFFFFFFBDu; // -0x43
constexpr uint32_t sys_wait_sema = 0x44;

constexpr uint32_t reg_v0 = 2, reg_v1 = 3, reg_a0 = 4, reg_a1 = 5, reg_a2 = 6, reg_a3 = 7, reg_t0 = 8;
constexpr uint32_t reg_t1 = 9, reg_sp = 29, reg_ra = 31;

// "addiu v1, zero, N; syscall" at `pc`, as the emitter writes it: pc = the next instruction, then the call;
// execution continues inline.
inline void syscall_at(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime, uint32_t pc, uint32_t number) {
    SET_GPR_U32(ctx, reg_v1, number);
    ctx->pc = pc + 4u;
    runtime->handleSyscall(rdram, ctx, 0x0u);
}

inline void jr_ra(R5900Context* ctx) {
    const uint32_t jumpTarget = GPR_U32(ctx, reg_ra);
    ctx->pc = jumpTarget;
}

} // namespace

// Function: start
// Address: 0x200000 - 0x200040
void start_0x200000(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    switch (ctx->pc) {
    case 0x200030u:
        goto label_200030;
    default:
        break;
    }
    ctx->pc = 0x200000u;
    SET_GPR_U32(ctx, reg_a0, 0u);
    SET_GPR_U32(ctx, reg_a1, main_stack);
    SET_GPR_U32(ctx, reg_a2, main_stack_size);
    SET_GPR_U32(ctx, reg_a3, 0u);
    SET_GPR_U32(ctx, reg_t0, 0u);
    syscall_at(rdram, ctx, runtime, 0x200018u, sys_setup_thread);
    ctx->pc = 0x200020u;
    SET_GPR_U32(ctx, reg_sp, GPR_U32(ctx, reg_v0));
    SET_GPR_U32(ctx, reg_ra, 0x200030u);
    ctx->pc = 0x200100u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x200100u, 0x200024u, 0x200030u,
                                      PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_200030:
    ctx->pc = 0x200030u;
    SET_GPR_U32(ctx, reg_a0, 0u);
    syscall_at(rdram, ctx, runtime, 0x200034u, sys_exit);
    ctx->pc = 0x20003cu;
}

// Function: main
// Address: 0x200100 - 0x200200
void main_0x200100(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    switch (ctx->pc) {
    case 0x200108u:
        goto label_200108;
    case 0x200180u:
        goto label_200180;
    case 0x2001a0u:
        goto label_2001a0;
    default:
        break;
    }
    ctx->pc = 0x200100u;
    SET_GPR_U32(ctx, reg_ra, 0x200108u);
    ctx->pc = 0x200300u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x200300u, 0x200100u, 0x200108u,
                                      PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_200108:
    ctx->pc = 0x200108u;
    SET_GPR_U32(ctx, reg_a0, sema_param);
    syscall_at(rdram, ctx, runtime, 0x200110u, sys_create_sema);
    WRITE32(var_vsync_sema, GPR_U32(ctx, reg_v0));
    SET_GPR_U32(ctx, reg_a0, sema_param);
    syscall_at(rdram, ctx, runtime, 0x200120u, sys_create_sema);
    WRITE32(var_work_sema, GPR_U32(ctx, reg_v0));
    SET_GPR_U32(ctx, reg_a0, 2u);
    SET_GPR_U32(ctx, reg_a1, 0x200400u);
    SET_GPR_U32(ctx, reg_a2, 0u);
    syscall_at(rdram, ctx, runtime, 0x200138u, sys_add_intc_handler);
    WRITE32(var_handler, GPR_U32(ctx, reg_v0));
    SET_GPR_U32(ctx, reg_a0, 2u);
    syscall_at(rdram, ctx, runtime, 0x200148u, sys_enable_intc);
    SET_GPR_U32(ctx, reg_a0, thread_param);
    syscall_at(rdram, ctx, runtime, 0x200154u, sys_create_thread);
    WRITE32(var_worker, GPR_U32(ctx, reg_v0));
    SET_GPR_U32(ctx, reg_a0, GPR_U32(ctx, reg_v0));
    SET_GPR_U32(ctx, reg_a1, 0u);
    syscall_at(rdram, ctx, runtime, 0x200168u, sys_start_thread);
    SET_GPR_U32(ctx, reg_ra, 0x200180u);
    ctx->pc = 0x200600u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x200600u, 0x200178u, 0x200180u,
                                      PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_200180:
    ctx->pc = 0x200180u;
    ctx->cop0_status |= 0x10000; // EI
label_200188:
    ctx->pc = 0x200188u;
    SET_GPR_U32(ctx, reg_a0, READ32(var_vsync_sema));
    syscall_at(rdram, ctx, runtime, 0x200190u, sys_wait_sema);
    SET_GPR_U32(ctx, reg_ra, 0x2001a0u);
    ctx->pc = 0x200500u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x200500u, 0x200198u, 0x2001a0u,
                                      PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_2001a0:
    ctx->pc = 0x2001a0u;
    SET_GPR_U32(ctx, reg_a0, READ32(var_work_sema));
    syscall_at(rdram, ctx, runtime, 0x2001a8u, sys_signal_sema);
    ctx->pc = 0x2001b0u;
    ctx->pc = 0x200188u;
    if (runtime->eeCheckpointDue()) {
        return;
    }
    goto label_200188;
}

// Function: gs_init
// Address: 0x200300 - 0x200380
void gs_init_0x200300(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = 0x200300u;
    SET_GPR_U32(ctx, reg_a0, 1u); // interlaced
    SET_GPR_U32(ctx, reg_a1, 2u); // NTSC
    SET_GPR_U32(ctx, reg_a2, 1u); // frame mode
    syscall_at(rdram, ctx, runtime, 0x200310u, sys_gs_set_crt);
    ctx->pc = 0x200318u;
    WRITE64(0x12000000u, UINT64_C(0x8025)); // PMODE: circuit 1, CRTMD, alpha from ALP, ALP = 0x80
    WRITE64(0x12000070u, UINT64_C(0x1400)); // DISPFB1: FBW 10 (640), PSMCT32, FBP 0
    WRITE64(0x12000080u, (UINT64_C(0x27C)) | (UINT64_C(0x32) << 12) | (UINT64_C(3) << 23) |
                             (UINT64_C(2559) << 32) | (UINT64_C(447) << 44)); // DISPLAY1
    WRITE32(0x1000E000u, 1u);                                                 // D_CTRL: DMA enable
    jr_ra(ctx);
}

// Function: vblank_handler (INTC 2)
// Address: 0x200400 - 0x200440
void vblank_handler_0x200400(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = 0x200400u;
    SET_GPR_U32(ctx, reg_t0, READ32(var_vblanks) + 1u);
    WRITE32(var_vblanks, GPR_U32(ctx, reg_t0));
    SET_GPR_U32(ctx, reg_a0, READ32(var_vsync_sema));
    syscall_at(rdram, ctx, runtime, 0x200418u, sys_isignal_sema);
    ctx->pc = 0x200420u;
    SET_GPR_U32(ctx, reg_v0, 0u);
    jr_ra(ctx);
}

// Function: draw_frame
// Address: 0x200500 - 0x200580
void draw_frame_0x200500(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    ctx->pc = 0x200500u;
    SET_GPR_U32(ctx, reg_t0, (READ32(var_frames) * 8u) & 0xFFu);
    WRITE32(gif_rgbaq, GPR_U32(ctx, reg_t0)); // R
    WRITE32(0x1000A010u, gif_packet);         // D2_MADR
    WRITE32(0x1000A020u, gif_packet_qwords);  // D2_QWC
    WRITE32(0x1000A000u, 0x101u);             // D2_CHCR: from memory, normal mode, STR
label_200520:
    ctx->pc = 0x200520u;
    SET_GPR_U32(ctx, reg_t1, READ32(0x1000A000u) & 0x100u);
    if (GPR_U32(ctx, reg_t1) != 0u) {
        ctx->pc = 0x200520u;
        if (runtime->eeCheckpointDue()) {
            return;
        }
        goto label_200520;
    }
    ctx->pc = 0x200530u;
    WRITE32(var_frames, READ32(var_frames) + 1u);
    jr_ra(ctx);
}

// Function: worker (thread entry)
// Address: 0x200580 - 0x2005c0
void worker_0x200580(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
label_200580:
    ctx->pc = 0x200580u;
    SET_GPR_U32(ctx, reg_a0, READ32(var_work_sema));
    syscall_at(rdram, ctx, runtime, 0x200588u, sys_wait_sema);
    ctx->pc = 0x200590u;
    WRITE32(var_work, READ32(var_work) + 1u);
    ctx->pc = 0x200580u;
    if (runtime->eeCheckpointDue()) {
        return;
    }
    goto label_200580;
}

// Function: read_file
// Address: 0x200600 - 0x200700
// PENDING (waits on M3 session C): the fileio read of disc_file through the SIF RPC client protocol
// (sceSifInitRpc, bind 0x80000001, open/read/close into var_file_word's buffer), written against session C's
// protocol layouts (tests/unit/support/sif_client.h, "Protocol notes" in Docs/milestones/M3-boots.md) once
// they are on main. Until then the function returns without touching the disc.
void read_file_0x200600(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    (void)rdram;
    (void)runtime;
    ctx->pc = 0x200600u;
    jr_ra(ctx);
}

// The unit's dense table, as register_functions.cpp declares it.
extern const uint32_t dq8_unit_machine_table_base = table_base;
extern const uint32_t dq8_unit_machine_table_end = table_end;
extern const uint32_t dq8_unit_machine_table_slot_count = (table_end - table_base) / 4u;
PS2Runtime::RecompiledFunction dq8_unit_machine_table[(table_end - table_base) / 4u] = {};

namespace {
struct GeneratedFunctionTableInitializer {
    static std::size_t slot(uint32_t addr) { return (addr - table_base) / 4u; }
    GeneratedFunctionTableInitializer() {
        dq8_unit_machine_table[slot(0x200000u)] = start_0x200000;
        dq8_unit_machine_table[slot(0x200030u)] = start_0x200000; // resume
        dq8_unit_machine_table[slot(0x200100u)] = main_0x200100;
        dq8_unit_machine_table[slot(0x200108u)] = main_0x200100; // resume
        dq8_unit_machine_table[slot(0x200180u)] = main_0x200100; // resume
        dq8_unit_machine_table[slot(0x2001a0u)] = main_0x200100; // resume
        dq8_unit_machine_table[slot(0x200300u)] = gs_init_0x200300;
        dq8_unit_machine_table[slot(0x200400u)] = vblank_handler_0x200400;
        dq8_unit_machine_table[slot(0x200500u)] = draw_frame_0x200500;
        dq8_unit_machine_table[slot(0x200580u)] = worker_0x200580;
        dq8_unit_machine_table[slot(0x200600u)] = read_file_0x200600;
    }
};
const GeneratedFunctionTableInitializer g_generatedFunctionTableInitializer;
} // namespace

const dq8_game_module_desc* machine_unit_descriptor() {
    static const dq8_game_module_desc desc{
        DQ8_GAME_MODULE_MAGIC,
        DQ8_GAME_MODULE_ABI_VERSION,
        sizeof(dq8_game_module_desc),
        DQ8_UNIT_MAIN,
        0,
        unit_name,
        "SYNTHETIC",
        source_sha1,
        "hand-written machine unit",
        sizeof(R5900Context),
        alignof(R5900Context),
        entry,
        dq8_unit_machine_table_base,
        dq8_unit_machine_table_end,
        dq8_unit_machine_table_slot_count,
        dq8_unit_machine_table,
    };
    return &desc;
}

extern "C" DQ8_MODULE_EXPORT const dq8_game_module_desc* dq8_game_module_describe(void) {
    return machine_unit_descriptor();
}
