// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_stop.h"
#include "runtime/iop/services/services.h"
#include "runtime/machine/machine_services.h"

#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A server that answers every call with zeros (ADR-0011): the game's sound servers until audio work starts.
// The first call of each function number is reported as an "rpc_null" event, so bring-up can see which
// replies the game might be checking.
namespace dq8::rt::iop_services {

namespace {

class NullServer final : public IopRpcServer {
  public:
    explicit NullServer(std::uint32_t sid) : sid_(sid), name_("null:" + hex32(sid)) {}

    std::uint32_t sid() const noexcept override { return sid_; }
    std::string_view name() const noexcept override { return name_; }
    std::uint32_t receive_buffer_size() const noexcept override { return 0x4000; }

    RpcReply call(IopCallContext& call, std::span<const std::uint8_t> args) override {
        if (seen_.insert(call.fno).second) {
            call.machine.emit(MachineEvent{"rpc_null", {}}
                                  .add("sid", std::uint64_t{sid_})
                                  .add("fno", std::uint64_t{call.fno})
                                  .add("send", std::uint64_t{args.size()})
                                  .add("recv", std::uint64_t{call.recv_size}));
        }
        return {std::vector<std::uint8_t>(call.recv_size, 0), {}, 0};
    }

  private:
    std::uint32_t sid_;
    std::string name_;
    std::set<std::uint32_t> seen_;
};

} // namespace

std::unique_ptr<IopRpcServer> make_null_server(std::uint32_t sid) {
    return std::make_unique<NullServer>(sid);
}

} // namespace dq8::rt::iop_services
