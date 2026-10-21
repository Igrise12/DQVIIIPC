// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/services/services.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The module loader (server id 0x80000006): a version query, then "load the module at this path with these
// arguments" ("Protocol notes", loadfile). Nothing is executed: the Iop registers the native services the
// region profile lists for the module and answers with a module id.
namespace dq8::rt::iop_services {

namespace {

constexpr std::uint32_t fn_module_load = 0;
constexpr std::uint32_t fn_version = 0xFF;
// The protocol version word the EE library compares before every load.
constexpr std::uint32_t protocol_version = 0x30303033u;

constexpr std::size_t load_arg_length = 0x000;
constexpr std::size_t load_path = 0x008;
constexpr std::size_t load_path_max = 252;
constexpr std::size_t load_args = load_path + load_path_max;
constexpr std::size_t load_args_max = 252;

// The reply: the module id (or a negative error), then the module's start result (0: stays resident).
constexpr std::size_t reply_result = 0x0;
constexpr std::size_t reply_start_result = 0x4;

class LoadfileServer final : public IopRpcServer {
  public:
    using Load = std::function<std::int32_t(std::string_view, std::span<const std::uint8_t>)>;
    explicit LoadfileServer(Load load) : load_(std::move(load)) {}

    std::uint32_t sid() const noexcept override { return sid_loadfile; }
    std::string_view name() const noexcept override { return "loadfile"; }
    std::uint32_t receive_buffer_size() const noexcept override { return 0x200; }

    RpcReply call(IopCallContext& call, std::span<const std::uint8_t> args) override {
        if (call.fno == fn_version) {
            return {words({protocol_version}), {}, 0};
        }
        if (call.fno != fn_module_load) {
            unsupported(call, name(), "not implemented");
        }
        if (!load_) {
            unsupported(call, name(), "no module loader is attached");
        }
        const std::string path = arg_string(args, load_path, load_path_max);
        const std::size_t arg_length = std::min<std::size_t>(arg32(args, load_arg_length), load_args_max);
        const auto module_args = args.size() >= load_args + arg_length ? args.subspan(load_args, arg_length)
                                                                       : std::span<const std::uint8_t>{};
        const std::int32_t id = load_(path, module_args);
        std::vector<std::uint8_t> reply(8, 0);
        put32(reply, reply_result, static_cast<std::uint32_t>(id));
        put32(reply, reply_start_result, 0);
        return {std::move(reply), {}, 0};
    }

  private:
    Load load_;
};

} // namespace

std::unique_ptr<IopRpcServer>
make_loadfile(std::function<std::int32_t(std::string_view, std::span<const std::uint8_t>)> load) {
    return std::make_unique<LoadfileServer>(std::move(load));
}

} // namespace dq8::rt::iop_services
