// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/io/disc_source.h"
#include "runtime/iop/iop_memory.h"
#include "runtime/iop/services/services.h"
#include "runtime/machine/cost_model.h"
#include "runtime/machine/machine_services.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The IOP heap server (server id 0x80000003): allocate and free IOP RAM for the EE, and load a disc file into
// an allocated block ("Protocol notes", iopheap). Allocations come from IopMemory's deterministic heap.
namespace dq8::rt::iop_services {

namespace {

constexpr std::uint32_t fn_alloc = 1;
constexpr std::uint32_t fn_free = 2;
constexpr std::uint32_t fn_load = 3;

constexpr std::size_t alloc_size = 0x0;
constexpr std::size_t free_address = 0x0;
constexpr std::size_t load_address = 0x0;
constexpr std::size_t load_path = 0x4;
constexpr std::size_t load_path_max = 252;

constexpr std::int32_t err_noent = 2;
constexpr std::int32_t err_nodev = 19;
constexpr std::int32_t err_inval = 22;

class IopHeapServer final : public IopRpcServer {
  public:
    explicit IopHeapServer(const IDiscSource* disc) : disc_(disc) {}

    std::uint32_t sid() const noexcept override { return sid_iopheap; }
    std::string_view name() const noexcept override { return "iopheap"; }
    std::uint32_t receive_buffer_size() const noexcept override { return 0x100; }

    RpcReply call(IopCallContext& call, std::span<const std::uint8_t> args) override {
        switch (call.fno) {
        case fn_alloc: {
            const auto block = call.iop.allocate(arg32(args, alloc_size));
            if (block) {
                blocks_.insert(*block);
            }
            return {words({block.value_or(0)}), {}, 0};
        }
        case fn_free: {
            const std::uint32_t addr = arg32(args, free_address) & 0x1FFFFFu;
            if (blocks_.erase(addr) == 0) {
                return {words({static_cast<std::uint32_t>(-1)}), {}, 0};
            }
            call.iop.free(addr);
            return {words({0}), {}, 0};
        }
        case fn_load:
            return load(call, args);
        default:
            unsupported(call, name(), "not implemented");
        }
    }

  private:
    // Reads a whole disc file into IOP RAM at the given address.
    RpcReply load(IopCallContext& call, std::span<const std::uint8_t> args) {
        const std::uint32_t addr = arg32(args, load_address) & 0x1FFFFFu;
        std::string device;
        const std::string full = arg_string(args, load_path, load_path_max);
        const std::string_view path = split_device(full, device);
        if (device != "cdrom0" && device != "cdrom") {
            return {words({static_cast<std::uint32_t>(-err_nodev)}), {}, 0};
        }
        const auto file = disc_ != nullptr ? disc_->find(path) : std::nullopt;
        if (!file) {
            return {words({static_cast<std::uint32_t>(-err_noent)}), {}, 0};
        }
        if (file->size > IopMemory::size - addr) {
            return {words({static_cast<std::uint32_t>(-err_inval)}), {}, 0};
        }
        const auto sectors = static_cast<std::uint32_t>((file->size + IDiscSource::sector_size - 1) /
                                                        IDiscSource::sector_size);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(sectors) * IDiscSource::sector_size);
        disc_->read_sectors(file->lsn, sectors, bytes);
        bytes.resize(static_cast<std::size_t>(file->size));
        call.iop.write(addr, bytes);
        return {words({0}), {}, call.machine.clock().now() + call.machine.cost().cdvd_read(bytes.size())};
    }

    const IDiscSource* disc_;
    std::set<std::uint32_t> blocks_; // what this server allocated, so a bad free fails as on the IOP
};

} // namespace

std::unique_ptr<IopRpcServer> make_iopheap(const IDiscSource* disc) {
    return std::make_unique<IopHeapServer>(disc);
}

} // namespace dq8::rt::iop_services
