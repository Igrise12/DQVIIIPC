// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/iop_rpc.h"

#include "runtime/ee/guest_stop.h"
#include "runtime/iop/iop_memory.h"
#include "runtime/iop/sif_link.h"
#include "runtime/iop/sif_protocol.h"
#include "runtime/machine/cost_model.h"
#include "runtime/machine/machine_services.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace dq8::rt {

namespace {

// The server data block: what the handle points at. The EE never reads it; we keep the server id and the
// receive buffer there so an IOP RAM dump shows which server a handle is.
constexpr std::uint32_t server_block_size = 0x40;
// The SIF delivers whole words and the EE side sends whole quadwords, so a receive buffer gets a quadword of
// slack beyond what the server asked for.
constexpr std::uint32_t receive_slack = 16;

} // namespace

IopRpcLayer::IopRpcLayer(MachineServices& machine, SifLink& link, IopMemory& iop, const IDiscSource* disc)
    : machine_(machine), link_(link), iop_(iop), disc_(disc) {}

IopRpcLayer::Server& IopRpcLayer::add(std::unique_ptr<IopRpcServer> server, std::string module) {
    const std::uint32_t sid = server->sid();
    if (by_sid_.count(sid) != 0) {
        throw std::invalid_argument("IOP RPC server " + hex32(sid) + " (" + std::string(server->name()) +
                                    ") is registered twice");
    }
    const std::uint32_t size = (std::max<std::uint32_t>(server->receive_buffer_size(), 16) + 15u) & ~15u;
    const auto handle = iop_.allocate(server_block_size);
    const auto buffer = iop_.allocate(size + receive_slack);
    if (!handle || !buffer) {
        fail("the IOP heap has no room for the " + std::string(server->name()) + " server (" + hex32(sid) +
             ")");
    }
    iop_.write32(*handle, sid);
    iop_.write32(*handle + 4, *buffer);
    Server entry;
    entry.impl = std::move(server);
    entry.module = std::move(module);
    entry.handle = *handle;
    entry.buffer = *buffer;
    entry.buffer_size = size + receive_slack;
    by_handle_[*handle] = sid;
    return by_sid_.emplace(sid, std::move(entry)).first->second;
}

const IopRpcLayer::Server* IopRpcLayer::find(std::uint32_t sid) const {
    const auto it = by_sid_.find(sid);
    return it == by_sid_.end() ? nullptr : &it->second;
}

std::vector<const IopRpcLayer::Server*> IopRpcLayer::servers() const {
    std::vector<const Server*> out;
    out.reserve(by_sid_.size());
    for (const auto& [sid, server] : by_sid_) {
        out.push_back(&server);
    }
    return out;
}

void IopRpcLayer::clear() {
    by_sid_.clear();
    by_handle_.clear();
}

void IopRpcLayer::bind(std::span<const std::uint8_t> packet) {
    ++binds_;
    const std::uint32_t sid = sif::load32(packet, sif::bind_sid);
    std::uint32_t handle = 0;
    std::uint32_t buffer = 0;
    if (const auto it = by_sid_.find(sid); it != by_sid_.end()) {
        handle = it->second.handle;
        buffer = it->second.buffer;
        machine_.emit(
            MachineEvent{"rpc_bind", {}}.add("sid", std::uint64_t{sid}).add("server", std::uint64_t{handle}));
    }
    machine_.trace(TraceCategory::rpc,
                   "bind " + hex32(sid) + (handle != 0 ? " -> " + hex32(handle) : " -> no server"));
    const Cycles ready = machine_.clock().now() + machine_.cost().rpc_reply(sif::end_size);
    link_.send_to_ee(end_packet(packet, sif::cid_rpc_bind, handle, buffer, 0, 0), {}, ready);
}

void IopRpcLayer::call(std::span<const std::uint8_t> packet) {
    ++calls_;
    const std::uint32_t handle = sif::load32(packet, sif::call_server);
    const auto hit = by_handle_.find(handle);
    if (hit == by_handle_.end()) {
        fail("an RPC call to server handle " + hex32(handle) + ", which is no registered server");
    }
    Server& s = by_sid_.at(hit->second);
    const std::uint32_t fno = sif::load32(packet, sif::call_fno);
    const std::uint32_t send_size = sif::load32(packet, sif::call_send_size);
    const std::uint32_t recv_addr = sif::load32(packet, sif::call_recv_addr);
    const std::uint32_t recv_size = sif::load32(packet, sif::call_recv_size);
    const std::uint32_t rmode = sif::load32(packet, sif::call_rmode);
    if (send_size > s.buffer_size) {
        fail("an RPC call to " + std::string(s.impl->name()) + " sends " + std::to_string(send_size) +
             " bytes; its receive buffer holds " + std::to_string(s.buffer_size));
    }
    std::vector<std::uint8_t> args(send_size);
    iop_.read(s.buffer, args);

    if (machine_.tracing(TraceCategory::rpc)) {
        machine_.emit(MachineEvent{"rpc_call", {}}
                          .add("sid", std::uint64_t{hit->second})
                          .add("fno", std::uint64_t{fno}));
    }
    IopCallContext ctx{machine_, iop_, disc_, hit->second, fno, recv_addr, recv_size};
    RpcReply reply;
    try {
        reply = s.impl->call(ctx, args);
    } catch (const GuestStop&) {
        throw;
    } catch (const std::exception& e) {
        fail(std::string(s.impl->name()) + " function " + std::to_string(fno) + " failed: " + e.what());
    }
    ++s.calls;

    std::vector<EeWrite> writes = std::move(reply.extra);
    std::uint32_t data_bytes = 0;
    if (recv_addr != 0 && recv_size != 0 && !reply.data.empty()) {
        data_bytes = static_cast<std::uint32_t>(std::min<std::size_t>(reply.data.size(), recv_size));
        reply.data.resize(data_bytes);
        writes.push_back(EeWrite{recv_addr, std::move(reply.data)});
    }
    std::uint64_t bytes = 0;
    for (const EeWrite& w : writes) {
        bytes += w.bytes.size();
    }
    const Cycles start = std::max(machine_.clock().now(), s.busy_until);
    const Cycles ready = std::max(start + machine_.cost().rpc_reply(bytes), reply.ready_at);
    s.busy_until = ready;
    if (rmode != 0) {
        link_.send_to_ee(end_packet(packet, sif::cid_rpc_call, s.handle, s.buffer, data_bytes, recv_addr),
                         std::move(writes), ready);
    } else {
        // No end packet was asked for: the data travels without an interrupt, followed by an end-format
        // packet written over the request itself with its slot flags and request number cleared, which frees
        // the EE's packet slot and ends sceSifCheckStatRpc's busy state.
        std::vector<std::uint8_t> release = end_packet(packet, sif::cid_rpc_call, s.handle, s.buffer, 0, 0);
        sif::store32(release, sif::rpc_rec_id, 0);
        sif::store32(release, sif::rpc_id, 0);
        writes.push_back(EeWrite{sif::load32(packet, sif::rpc_pkt_addr), std::move(release)});
        link_.send_to_ee({}, std::move(writes), ready);
    }
}

void IopRpcLayer::rdata(std::span<const std::uint8_t> packet) {
    const std::uint32_t src = sif::load32(packet, sif::rdata_src) & 0x1FFFFFFFu & (IopMemory::size - 1);
    const std::uint32_t dest = sif::load32(packet, sif::rdata_dest);
    const std::uint32_t size = sif::load32(packet, sif::rdata_size);
    if (size > IopMemory::size - src) {
        fail("an RPC data request for " + std::to_string(size) + " bytes at IOP " + hex32(src) +
             " runs past IOP RAM");
    }
    std::vector<EeWrite> writes;
    if (size != 0) {
        EeWrite w{dest, std::vector<std::uint8_t>(size)};
        iop_.read(src, w.bytes);
        writes.push_back(std::move(w));
    }
    const Cycles ready = machine_.clock().now() + machine_.cost().rpc_reply(size);
    link_.send_to_ee(end_packet(packet, sif::cid_rpc_rdata, 0, 0, size, dest), std::move(writes), ready);
}

std::vector<std::uint8_t> IopRpcLayer::end_packet(std::span<const std::uint8_t> request,
                                                  std::uint32_t ended_cid, std::uint32_t server,
                                                  std::uint32_t buffer, std::uint32_t extra_size,
                                                  std::uint32_t extra_dest) const {
    std::vector<std::uint8_t> p(sif::end_size, 0);
    sif::store32(p, sif::hdr_sizes, sif::end_size | (extra_size << 8));
    sif::store32(p, sif::hdr_extra_dest, extra_dest);
    sif::store32(p, sif::hdr_cid, sif::cid_rpc_end);
    for (const std::uint32_t off : {sif::rpc_rec_id, sif::rpc_pkt_addr, sif::rpc_id, sif::rpc_client}) {
        sif::store32(p, off, sif::load32(request, off));
    }
    sif::store32(p, sif::end_cid, ended_cid);
    sif::store32(p, sif::end_server, server);
    sif::store32(p, sif::end_buffer, buffer);
    return p;
}

void IopRpcLayer::fail(std::string message) const {
    StopInfo info;
    info.reason = StopReason::guest_error;
    info.message = "iop: " + std::move(message);
    machine_.stop(std::move(info));
    std::abort();
}

} // namespace dq8::rt
