// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// The machine unit (machine_unit.h), written by hand in the emitter's shape. The guest program, all ours
// (one line per guest function; syscall numbers in v1):
//
//   0x00200000 start:       SetupThread(0, main_stack, main_stack_size, 0, 0); sp = v0; jal main; Exit
//   0x00200100 main:        jal gs_init; vsync_sema = CreateSema(&sema_param); work_sema = CreateSema(...);
//                           handler = AddIntcHandler(2, vblank_handler, 0); EnableIntc(2);
//                           worker = CreateThread(&thread_param); StartThread(worker, 0); EI; jal read_file;
//                           loop: WaitSema(vsync_sema); jal draw_frame; SignalSema(work_sema); b loop
//   0x00200300 gs_init:     GsSetCrt(1, 2, 1); PMODE, DISPFB1, DISPLAY1 (sd); D_CTRL = 1; jr ra
//   0x00200400 vblank_handler: vblanks += 1; iSignalSema(vsync_sema); v0 = 0; jr ra
//   0x00200500 draw_frame:  rgbaq.r = frames * 8; D2_MADR = gif_packet; D2_QWC = 9; D2_CHCR = 0x101;
//                           wait: if (D2_CHCR & 0x100) b wait; frames += 1; jr ra
//   0x00200580 worker:      loop: WaitSema(work_sema); work += 1; b loop
//   0x00200600 read_file:   the SIF command and RPC layers' init, bind fileio, setup, open, read, close
//   (below) 0x00200700 sif0_handler: DMAC channel 5: one IOP packet per interrupt from the receive slot
#include "support/machine_unit.h"

#include "ps2_runtime_macros.h"

#include <initializer_list>
#include <optional>

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
constexpr uint32_t sys_add_dmac_handler = 0x12;
constexpr uint32_t sys_enable_dmac = 0x16;
constexpr uint32_t sys_sif_dma_stat = 0x76;
constexpr uint32_t sys_sif_set_dma = 0x77;
constexpr uint32_t sys_sif_set_dchain = 0x78;
constexpr uint32_t sys_isif_set_dchain = 0xFFFFFF88u; // -0x78
constexpr uint32_t sys_sif_set_reg = 0x79;
constexpr uint32_t sys_sif_get_reg = 0x7A;

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
    ctx->pc = 0x200170u;
    ctx->cop0_status |= 0x10000; // EI
    SET_GPR_U32(ctx, reg_ra, 0x200180u);
    ctx->pc = 0x200600u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x200600u, 0x200178u, 0x200180u,
                                      PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;
    }
label_200180:
    ctx->pc = 0x200180u;
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

// The SIF client (read_file and sif0_handler). It speaks the protocol of Docs/milestones/M3-boots.md
// ("Protocol notes") the way the game's EE libraries do. It is written as structured C++ rather than one
// statement per instruction, over the same PS2Runtime surface the emitter uses: guest loads and stores,
// syscalls with the number in v1, and a back-edge checkpoint in every wait loop.
namespace {

constexpr uint32_t uncached = 0x20000000u;
constexpr uint32_t sid_fileio = 0x80000001u;
constexpr uint32_t cid_change_saddr = 0x80000000u;
constexpr uint32_t cid_set_sreg = 0x80000001u;
constexpr uint32_t cid_init = 0x80000002u;
constexpr uint32_t cid_rpc_end = 0x80000008u;
constexpr uint32_t cid_rpc_bind = 0x80000009u;
constexpr uint32_t cid_rpc_call = 0x8000000Au;
constexpr uint32_t cid_fileio_done = 0x80000011u;
constexpr uint32_t smflg_cmd_ready = 0x20000u;
constexpr uint32_t fio_version = 0x30303033u;

struct Guest {
    uint8_t* rdram;
    R5900Context* ctx;
    PS2Runtime* runtime;

    uint32_t r32(uint32_t addr) { return READ32(addr); }
    void w32(uint32_t addr, uint32_t value) { WRITE32(addr, value); }
    uint8_t r8(uint32_t addr) { return READ8(addr); }
    void w8(uint32_t addr, uint8_t value) { WRITE8(addr, value); }
    void copy(uint32_t dest, uint32_t src, uint32_t bytes) {
        for (uint32_t i = 0; i < bytes; i += 4) {
            w32(dest + i, r32(src + i));
        }
    }
    void zero(uint32_t dest, uint32_t bytes) {
        for (uint32_t i = 0; i < bytes; i += 4) {
            w32(dest + i, 0);
        }
    }
    // A syscall at `pc` with up to six arguments in a0..a3, t0, t1; returns v0.
    uint32_t sys(uint32_t pc, uint32_t number, std::initializer_list<uint32_t> args = {}) {
        int reg = static_cast<int>(reg_a0);
        for (const uint32_t a : args) {
            SET_GPR_U32(ctx, reg, a);
            reg = reg == static_cast<int>(reg_a3) ? static_cast<int>(reg_t0) : reg + 1;
        }
        ctx->pc = pc;
        syscall_at(rdram, ctx, runtime, pc, number);
        return GPR_U32(ctx, reg_v0);
    }
    // The back-edge of a wait loop at `pc`: the checkpoint the emitter puts on every backward branch.
    void back_edge(uint32_t pc) {
        ctx->pc = pc;
        (void)runtime->eeCheckpointDue();
    }

    // Sends a command packet of `size` bytes at `packet`, with `extra_size` bytes from `extra_src` ahead of
    // it to `extra_dest`, and waits until the transfer is done (sceSifDmaStat < 0).
    void send(uint32_t pc, uint32_t cid, uint32_t packet, uint32_t size, uint32_t opt, uint32_t extra_src = 0,
              uint32_t extra_dest = 0, uint32_t extra_size = 0) {
        w32(packet + 0x0, size | (extra_size << 8));
        w32(packet + 0x4, extra_size != 0 ? extra_dest : 0);
        w32(packet + 0x8, cid);
        w32(packet + 0xC, opt);
        uint32_t n = 0;
        if (extra_size != 0) {
            descriptor(n++, extra_src, extra_dest, extra_size, 0);
        }
        descriptor(n++, packet, r32(var_iop_buffer), size, 0x44);
        const uint32_t id = sys(pc, sys_sif_set_dma, {sif_desc, n});
        while (static_cast<int32_t>(sys(pc + 8, sys_sif_dma_stat, {id})) >= 0) {
            back_edge(pc + 8);
        }
    }
    void descriptor(uint32_t i, uint32_t src, uint32_t dest, uint32_t size, uint32_t attr) {
        const uint32_t at = sif_desc + 16 * i;
        w32(at + 0x0, src);
        w32(at + 0x4, dest);
        w32(at + 0x8, size);
        w32(at + 0xC, attr);
    }
    void send_word(uint32_t pc, uint32_t cid, uint32_t opt, uint32_t word) {
        zero(sif_cmd_packet, 0x20);
        w32(sif_cmd_packet + 0x10, word);
        send(pc, cid, sif_cmd_packet, 0x14, opt);
    }

    // The one RPC packet slot, filled in for a new request: flags (allocated), its own uncached address and
    // the next request number. Returns the uncached address.
    uint32_t alloc_packet() {
        zero(sif_rpc_packet, 0x40);
        const uint32_t id = r32(var_rpc_id) + 1u;
        w32(var_rpc_id, id);
        w32(sif_rpc_packet + 0x10, 5u);
        w32(sif_rpc_packet + 0x14, sif_rpc_packet | uncached);
        w32(sif_rpc_packet + 0x18, id);
        return sif_rpc_packet | uncached;
    }
    // sceSifCheckStatRpc's test.
    bool call_done(uint32_t client) {
        const uint32_t packet = r32(client) & 0x1FFFFFFFu;
        return packet == 0 || r32(client + 0x04) != r32(packet + 0x18) || (r32(packet + 0x10) & 1u) == 0;
    }
    uint32_t bind(uint32_t pc, uint32_t client, uint32_t sid) {
        const uint32_t packet = alloc_packet();
        zero(client, 0x28);
        w32(client + 0x00, packet);
        w32(client + 0x04, r32(packet + 0x18));
        w32(client + 0x08, 0xFFFFFFFFu);
        w32(packet + 0x1C, client);
        w32(packet + 0x20, sid);
        send(pc, cid_rpc_bind, packet, 0x40, 0);
        while (r32(client) != 0) {
            back_edge(pc + 0x10);
        }
        return r32(client + 0x24);
    }
    void call(uint32_t pc, uint32_t client, uint32_t fno, uint32_t send_addr, uint32_t send_size,
              uint32_t recv, uint32_t recv_size) {
        const uint32_t packet = alloc_packet();
        w32(client + 0x00, packet);
        w32(client + 0x04, r32(packet + 0x18));
        w32(packet + 0x1C, client);
        w32(packet + 0x20, fno);
        w32(packet + 0x24, send_size);
        w32(packet + 0x28, recv);
        w32(packet + 0x2C, recv_size);
        w32(packet + 0x30, 1u); // rmode: wait for the end packet
        w32(packet + 0x34, r32(client + 0x24));
        send(pc, cid_rpc_call, packet, 0x40, 0, send_addr, r32(client + 0x14), send_size);
        while (!call_done(client)) {
            back_edge(pc + 0x10);
        }
    }
    // One fileio request: the call ("accepted"), then the completion record; returns the result word.
    std::optional<int32_t> fileio(uint32_t pc, uint32_t fno, uint32_t request, uint32_t size) {
        w32(request + 0x00, 0);          // EE semaphore: we poll instead
        w32(request + 0x04, fio_result); // result pointer
        w32(request + 0x08, 4);          // result size
        const uint32_t before = r32(var_completions);
        call(pc, sif_client, fno, request, size, fio_reply, 4);
        if (r32(fio_reply) == 0) {
            return std::nullopt;
        }
        while (r32(var_completions) == before) {
            back_edge(pc + 0x20);
        }
        return static_cast<int32_t>(r32(fio_result));
    }
};

} // namespace

// Function: read_file
// Address: 0x200600 - 0x200700
void read_file_0x200600(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    Guest g{rdram, ctx, runtime};
    ctx->pc = 0x200600u;
    // The receive side: the SIF0 handler on DMAC channel 5.
    g.sys(0x200600u, sys_add_dmac_handler, {5u, 0x200700u, 0u, 0u});
    g.sys(0x200608u, sys_enable_dmac, {5u});
    // Command-layer init.
    if ((g.r32(0x1000E010u) & (1u << 5)) != 0) { // D_STAT CIS5
        g.w32(0x1000E010u, 1u << 5);
    }
    if ((g.r32(0x1000C000u) & 0x100u) == 0) { // D5_CHCR STR
        g.sys(0x200610u, sys_sif_set_dchain);
    }
    const uint32_t known = g.sys(0x200618u, sys_sif_get_reg, {0x80000000u});
    if (known != 0) {
        g.w32(var_iop_buffer, known);
        g.send_word(0x200620u, cid_change_saddr, 0, sif_recv_slot);
    } else {
        while ((g.sys(0x200628u, sys_sif_get_reg, {4u}) & smflg_cmd_ready) == 0) {
            g.back_edge(0x200628u);
        }
        g.w32(var_iop_buffer, g.sys(0x200630u, sys_sif_get_reg, {2u}));
        g.sys(0x200638u, sys_sif_set_reg, {0x80000000u, g.r32(var_iop_buffer)});
        g.sys(0x200640u, sys_sif_set_reg, {0x80000001u, sif_cmd_data});
        g.send_word(0x200648u, cid_init, 0, sif_recv_slot);
    }
    // RPC-layer init: init with option 1, then wait for the IOP to set our software register 0.
    if (g.sys(0x200650u, sys_sif_get_reg, {0x80000002u}) == 0) {
        g.zero(sif_cmd_packet, 0x10);
        g.send(0x200658u, cid_init, sif_cmd_packet, 0x10, 1);
        while (g.r32(sreg_mirror) == 0) {
            g.back_edge(0x200660u);
        }
        g.sys(0x200668u, sys_sif_set_reg, {0x80000002u, 1u});
    }
    // Bind fileio (every bind loop of the game retries until the server word is non-zero).
    uint32_t server = 0;
    for (int tries = 0; tries < 16 && server == 0; ++tries) {
        server = g.bind(0x200670u, sif_client, sid_fileio);
    }
    if (server == 0) {
        g.w32(var_file_status, static_cast<uint32_t>(file_status_bind));
        jr_ra(ctx);
        return;
    }
    // Setup: the two completion records; the reply's first word is the protocol version.
    g.w32(fio_setup_args + 0, fio_records);
    g.w32(fio_setup_args + 4, fio_records + 0x440);
    g.call(0x200680u, sif_client, 0xFF, fio_setup_args, 8, fio_reply, 8);
    if (g.r32(fio_reply) != fio_version) {
        g.w32(var_file_status, static_cast<uint32_t>(file_status_setup));
        jr_ra(ctx);
        return;
    }
    // open (function 0): flags 1, the path at +0x14.
    g.zero(fio_request, 0x418);
    g.w32(fio_request + 0x0C, 1u);
    for (uint32_t i = 0; disc_file_path[i] != '\0'; ++i) {
        g.w8(fio_request + 0x14 + i, static_cast<uint8_t>(disc_file_path[i]));
    }
    const auto fd = g.fileio(0x200690u, 0, fio_request, 0x418);
    if (!fd || *fd < 0) {
        g.w32(var_file_status, static_cast<uint32_t>(fd ? *fd : file_status_call));
        jr_ra(ctx);
        return;
    }
    // read (function 2): fd, EE buffer, size; then close (function 1).
    g.zero(fio_request, 0x20);
    g.w32(fio_request + 0x0C, static_cast<uint32_t>(*fd));
    g.w32(fio_request + 0x10, fio_buffer);
    g.w32(fio_request + 0x14, fio_read_size);
    const auto n = g.fileio(0x2006A0u, 2, fio_request, 0x20);
    g.zero(fio_request, 0x14);
    g.w32(fio_request + 0x0C, static_cast<uint32_t>(*fd));
    (void)g.fileio(0x2006B0u, 1, fio_request, 0x14);
    g.w32(var_file_word, g.r32(fio_buffer));
    g.w32(var_file_status, static_cast<uint32_t>(n ? *n : file_status_call));
    ctx->pc = 0x2006C0u;
    jr_ra(ctx);
}

// Function: sif0_handler (DMAC channel 5)
// Address: 0x200700 - 0x200800
void sif0_handler_0x200700(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    Guest g{rdram, ctx, runtime};
    ctx->pc = 0x200700u;
    g.w32(var_sif0_irqs, g.r32(var_sif0_irqs) + 1u);
    const uint8_t size = g.r8(sif_recv_slot);
    if (size != 0) {
        // Take the packet out of the slot, free the slot, re-arm SIF0, then act on the packet.
        g.w8(sif_recv_slot, 0);
        g.copy(sif_scratch, sif_recv_slot, 0x80);
        g.sys(0x200710u, sys_isif_set_dchain);
        const uint32_t cid = g.r32(sif_scratch + 0x08);
        if (cid == cid_change_saddr) {
            g.w32(var_iop_buffer, g.r32(sif_scratch + 0x10));
        } else if (cid == cid_set_sreg) {
            const uint32_t index = g.r32(sif_scratch + 0x10);
            if (index < 32) {
                g.w32(sreg_mirror + 4 * index, g.r32(sif_scratch + 0x14));
            }
        } else if (cid == cid_rpc_end) {
            const uint32_t client = g.r32(sif_scratch + 0x1C);
            if (g.r32(sif_scratch + 0x20) == cid_rpc_bind) {
                g.w32(client + 0x24, g.r32(sif_scratch + 0x24)); // server
                g.w32(client + 0x14, g.r32(sif_scratch + 0x28)); // server receive buffer
            }
            const uint32_t packet = g.r32(client) & 0x1FFFFFFFu;
            if (packet != 0) {
                g.w32(packet + 0x18, 0);
                g.w32(packet + 0x10, g.r32(packet + 0x10) & ~1u);
            }
            g.w32(client, 0);
        } else if (cid == cid_fileio_done) {
            // The record the option word names: copy its result to the request's result pointer.
            const uint32_t record = fio_records + 0x440u * (g.r32(sif_scratch + 0x0C) & 1u);
            g.copy(g.r32(record + 0x08), record + 0x10, g.r32(record + 0x0C));
            g.w32(var_completions, g.r32(var_completions) + 1u);
        }
    }
    ctx->pc = 0x200740u;
    SET_GPR_U32(ctx, reg_v0, 0u);
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
        dq8_unit_machine_table[slot(0x200700u)] = sif0_handler_0x200700;
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
