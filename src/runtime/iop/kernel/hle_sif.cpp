// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/iop_exec.h"
#include "runtime/iop/kernel/hle.h"

// sifman and sifcmd as HLE (Docs/milestones/M4-playable.md, session A; "Protocol notes" has the structures
// and packets). Servers a module registers are served by IopRpcLayer like the native ones; their calls are
// queued for the thread in sceSifRpcLoop. The IOP's own binds and calls go to EE servers through the SIF
// link.
namespace dq8::rt {

namespace {

// ---- sifman

void sif_set_dma(HleCall& c) {
    c.ret(c.exec().sif_dma(c, c.arg(0), c.arg(1)));
}

void sif_dma_stat(HleCall& c) {
    c.ret(c.exec().sif_dma_status(c.arg(0)));
}

// ---- sifcmd

void sif_init_rpc(HleCall&) {}

void sif_bind_rpc(HleCall& c) {
    c.exec().client_bind(c, c.arg(0), c.arg(1), c.arg(2));
}

void sif_call_rpc(HleCall& c) {
    c.exec().client_call(c, c.arg(0), c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5), c.arg(6), c.arg(7),
                         c.arg(8));
}

void sif_register_rpc(HleCall& c) {
    IopGuestServer s;
    s.data = c.arg(0);
    s.sid = c.arg(1);
    s.function = c.arg(2);
    s.buffer = c.arg(3);
    s.cfunction = c.arg(4);
    s.cbuffer = c.arg(5);
    s.queue = c.arg(6);
    s.gp = c.caller_gp();
    s.module = c.exec().module_at(c.cpu().gpr[31]);
    if (s.function == 0 || s.queue == 0) {
        c.fail("a server with no function or no queue");
    }
    // The fields of the server structure the IOP library fills in.
    c.write32(s.data + 0x00, s.sid);
    c.write32(s.data + 0x04, s.function);
    c.write32(s.data + 0x08, s.buffer);
    c.write32(s.data + 0x10, s.cfunction);
    c.write32(s.data + 0x14, s.cbuffer);
    c.write32(s.data + 0x40, s.queue);
    c.exec().register_server(s);
    c.ret(s.data);
}

void sif_set_rpc_queue(HleCall& c) {
    const std::uint32_t queue = c.arg(0);
    const std::uint32_t thread = c.arg(1);
    for (std::uint32_t off = 0; off < 0x18; off += 4) {
        c.write32(queue + off, 0);
    }
    c.write32(queue + 0x00, thread);
    IopRpcQueue& q = c.exec().rpc_queues()[queue];
    q.address = queue;
    q.thread = static_cast<std::int32_t>(thread);
}

void sif_rpc_loop(HleCall& c) {
    if (c.in_interrupt()) {
        c.fail("called from the interrupt context");
    }
    c.exec().rpc_loop(*c.thread(), c.arg(0));
}

void sif_remove_rpc(HleCall& c) {
    c.exec().remove_server(c.arg(0));
    c.ret(c.arg(0));
}

void sif_remove_rpc_queue(HleCall& c) {
    c.exec().rpc_queues().erase(c.arg(0));
    c.ret(c.arg(0));
}

constexpr HleFunction sifman_table[] = {
    {"sifman", 7, "sceSifSetDma", sif_set_dma},
    {"sifman", 8, "sceSifDmaStat", sif_dma_stat},
};

constexpr HleFunction sifcmd_table[] = {
    {"sifcmd", 14, "sceSifInitRpc", sif_init_rpc},
    {"sifcmd", 15, "sceSifBindRpc", sif_bind_rpc},
    {"sifcmd", 16, "sceSifCallRpc", sif_call_rpc},
    {"sifcmd", 17, "sceSifRegisterRpc", sif_register_rpc},
    {"sifcmd", 19, "sceSifSetRpcQueue", sif_set_rpc_queue},
    {"sifcmd", 22, "sceSifRpcLoop", sif_rpc_loop},
    {"sifcmd", 24, "sceSifRemoveRpc", sif_remove_rpc},
    {"sifcmd", 25, "sceSifRemoveRpcQueue", sif_remove_rpc_queue},
};

} // namespace

std::span<const HleFunction> hle_sifman() {
    return sifman_table;
}

std::span<const HleFunction> hle_sifcmd() {
    return sifcmd_table;
}

} // namespace dq8::rt
