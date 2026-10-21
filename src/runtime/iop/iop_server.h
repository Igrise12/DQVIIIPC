// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/machine/clock.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// A native SIF RPC server (architecture §5.2 stage B, ADR-0011): the EE's own RPC library binds it by server
// id and calls it with a function number and an argument buffer; the server answers with reply bytes that the
// SIF link writes to the EE's receive buffer at a virtual time, followed by the RPC end packet and a SIF0
// DMAC interrupt. Implemented by M3 session C (fileio, iopheap, loadfile, cdvdfsv, padman, mcserv, null
// servers).
namespace dq8::rt {

class MachineServices;
class IopMemory;
class IDiscSource;

struct EeWrite {
    std::uint32_t address = 0; // EE physical address
    std::vector<std::uint8_t> bytes;
};

struct RpcReply {
    std::vector<std::uint8_t> data; // written at the call's receive address, up to its receive size
    std::vector<EeWrite> extra;     // other EE memory a server fills itself (fileio and cdvd reads)
    Cycles ready_at = 0;            // absolute virtual time; 0 = now + CostModel::rpc_reply(bytes)
};

struct IopCallContext {
    MachineServices& machine;
    IopMemory& iop;
    const IDiscSource* disc = nullptr;
    std::uint32_t sid = 0;
    std::uint32_t fno = 0;
    std::uint32_t recv_address = 0; // EE address of the reply buffer
    std::uint32_t recv_size = 0;
};

class IopRpcServer {
  public:
    virtual ~IopRpcServer() = default;
    virtual std::uint32_t sid() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;
    // IOP RAM the server's receive buffer needs (where the EE's argument data lands).
    virtual std::uint32_t receive_buffer_size() const noexcept = 0;
    virtual RpcReply call(IopCallContext& call, std::span<const std::uint8_t> args) = 0;
    virtual void reset() {}
};

} // namespace dq8::rt
