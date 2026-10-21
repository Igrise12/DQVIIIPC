// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/iop/iop_server.h"
#include "runtime/machine/clock.h"

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

// The IOP's side of SIF RPC (architecture §5.2, ADR-0011): the registered servers by server id, and the bind,
// call and "other data" requests as they land in the IOP's command buffer. Each server gets two blocks of IOP
// RAM: a small data block whose address is the server handle the EE keeps after a bind, and the receive
// buffer the EE sends call arguments to. A bind to a server id nobody registered answers server 0, which
// makes the EE's library bind again later. A reply becomes ready at a virtual time (CostModel::rpc_reply, or
// later when the server says so) and one server answers one call at a time, as an IOP server thread does.
namespace dq8::rt {

class MachineServices;
class SifLink;
class IopMemory;
class IDiscSource;

class IopRpcLayer {
  public:
    struct Server {
        std::unique_ptr<IopRpcServer> impl;
        std::string module;            // the module that registered it; empty for a standard service
        std::uint32_t handle = 0;      // IOP address of the server's data block
        std::uint32_t buffer = 0;      // IOP address of its receive buffer
        std::uint32_t buffer_size = 0; // usable bytes at `buffer`
        Cycles busy_until = 0;         // when its last reply is ready
        std::uint64_t calls = 0;
    };

    IopRpcLayer(MachineServices& machine, SifLink& link, IopMemory& iop, const IDiscSource* disc);

    // Registers a server and allocates its IOP RAM. std::invalid_argument for a server id that is already
    // registered; a full IOP heap stops the guest.
    Server& add(std::unique_ptr<IopRpcServer> server, std::string module);
    const Server* find(std::uint32_t sid) const;
    // Every server, sorted by server id.
    std::vector<const Server*> servers() const;
    // Forgets every server (an IOP reboot; the caller resets IOP RAM).
    void clear();

    // The RPC commands, as they land.
    void bind(std::span<const std::uint8_t> packet);
    void call(std::span<const std::uint8_t> packet);
    void rdata(std::span<const std::uint8_t> packet);

    std::uint64_t binds() const noexcept { return binds_; }
    std::uint64_t calls() const noexcept { return calls_; }

  private:
    // The end packet for `request`: the header, the request's echoed words, the command it ends, and the
    // server and buffer words the EE stores after a bind.
    std::vector<std::uint8_t> end_packet(std::span<const std::uint8_t> request, std::uint32_t ended_cid,
                                         std::uint32_t server, std::uint32_t buffer, std::uint32_t extra_size,
                                         std::uint32_t extra_dest) const;
    [[noreturn]] void fail(std::string message) const;

    MachineServices& machine_;
    SifLink& link_;
    IopMemory& iop_;
    const IDiscSource* disc_;
    std::map<std::uint32_t, Server> by_sid_;
    std::map<std::uint32_t, std::uint32_t> by_handle_; // handle -> sid
    std::uint64_t binds_ = 0;
    std::uint64_t calls_ = 0;
};

} // namespace dq8::rt
