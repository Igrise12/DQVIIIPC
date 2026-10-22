// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors: ps2xIOP/src/modules/mcserv.cpp.
// Changes: the service runs on IopRpcServer (arguments arrive as the call's bytes, replies and the EE-side
// blocks are returned instead of written through a host interface); the parameter structs are offset
// tables; the host memory-card backend is replaced by the card model of runtime/io/memcard (one store per
// port, IopServiceDeps::cards); only the function numbers this game's libmc sends are served, every other one
// stops; replies take the cost model's memory-card time.
#include "runtime/ee/guest_memory.h"
#include "runtime/io/memcard/memory_card.h"
#include "runtime/io/memcard/memory_card_store.h"
#include "runtime/iop/iop_modules.h"
#include "runtime/iop/services/services.h"
#include "runtime/machine/cost_model.h"
#include "runtime/machine/machine_services.h"
#include "runtime/machine/rtc.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The memory-card server (server id 0x80000400; Docs/milestones/M4-playable.md, "Protocol notes", mcserv).
// Every call but init is a no-wait call answered with one result word after the card's latency; some also
// fill EE memory (GetInfo's info block, Read's data and fix-up block, GetDir's table). The card in each port
// is an IMemoryCardStore (null: no card); the virtual RTC stamps new and changed entries.
namespace dq8::rt::iop_services {

namespace {

enum class Operation {
    Init,
    GetInfo,
    Open,
    Close,
    Seek,
    Read,
    Write,
    Flush,
    Chdir,
    GetDir,
    SetInfo,
    Delete,
    Format,
    Unformat,
};

// The function numbers of the game's libmc (init 0xFE). Anything else stops.
std::optional<Operation> decode_operation(std::uint32_t function) {
    switch (function) {
    case 0xFEu:
        return Operation::Init;
    case 0x01u:
        return Operation::GetInfo;
    case 0x02u:
        return Operation::Open;
    case 0x03u:
        return Operation::Close;
    case 0x04u:
        return Operation::Seek;
    case 0x05u:
        return Operation::Read;
    case 0x06u:
        return Operation::Write;
    case 0x0Au:
        return Operation::Flush;
    case 0x0Cu:
        return Operation::Chdir;
    case 0x0Du:
        return Operation::GetDir;
    case 0x0Eu:
        return Operation::SetInfo;
    case 0x0Fu:
        return Operation::Delete;
    case 0x10u:
        return Operation::Format;
    case 0x11u:
        return Operation::Unformat;
    default:
        return std::nullopt;
    }
}

bool uses_name_parameter(Operation operation) {
    return operation == Operation::Open || operation == Operation::Chdir || operation == Operation::GetDir ||
           operation == Operation::SetInfo || operation == Operation::Delete;
}

// Descriptor requests (handle and port calls), 0x30 bytes.
constexpr std::size_t desc_fd = 0x00;
constexpr std::size_t desc_port = 0x04;
constexpr std::size_t desc_size = 0x0C;   // Read and Write: bytes (Write: past the inline head)
constexpr std::size_t desc_offset = 0x10; // Seek
constexpr std::size_t desc_origin = 0x14; // Seek: origin; Write: inline head bytes
constexpr std::size_t desc_buffer = 0x18; // Read and Write: EE buffer
constexpr std::size_t desc_param = 0x1C;  // GetInfo: the info block; Read: the fix-up block
constexpr std::size_t desc_inline = 0x20; // Write: up to 16 head bytes
constexpr std::size_t desc_inline_max = 16;
constexpr std::size_t desc_bytes = 0x30;
// Name requests (path calls), 0x414 bytes.
constexpr std::size_t name_port = 0x00;
constexpr std::size_t name_flags = 0x08;  // Open: flags; GetDir: mode; SetInfo: flags
constexpr std::size_t name_maxent = 0x0C; // GetDir
constexpr std::size_t name_table = 0x10;  // GetDir: the EE table; SetInfo: the EE info record; Chdir: output
constexpr std::size_t name_path = 0x14;
constexpr std::size_t name_path_max = 0x400;
constexpr std::size_t name_bytes = 0x414;

// GetInfo's EE block: type at +0, free clusters at +4, format state at +0x90, 0xC0 bytes in all.
constexpr std::size_t info_type = 0x00;
constexpr std::size_t info_free = 0x04;
constexpr std::size_t info_format = 0x90;
constexpr std::size_t info_block = 0xC0;
// Read's fix-up block starts with the unaligned head and tail counts and their destinations; all zero when
// the server writes every byte into the EE buffer itself.
constexpr std::size_t fixup_header = 16;

// The init reply's version words: this game's library needs mcserv >= 0x20A and mcman >= 0x20E.
constexpr std::uint32_t mcserv_version = 0x020Au;
constexpr std::uint32_t mcman_version = 0x020Eu;

constexpr std::uint32_t known_open_flags =
    mc::open_access | mc::open_mkdir | mc::open_create | mc::open_truncate;
constexpr std::uint32_t known_info_flags =
    mc::info_created | mc::info_modified | mc::info_attr | mc::info_rename;

std::string_view operation_name(Operation op, std::uint32_t flags) {
    switch (op) {
    case Operation::Init:
        return "init";
    case Operation::GetInfo:
        return "getinfo";
    case Operation::Open:
        return (flags & mc::open_mkdir) != 0 ? "mkdir" : "open";
    case Operation::Close:
        return "close";
    case Operation::Seek:
        return "seek";
    case Operation::Read:
        return "read";
    case Operation::Write:
        return "write";
    case Operation::Flush:
        return "flush";
    case Operation::Chdir:
        return "chdir";
    case Operation::GetDir:
        return "getdir";
    case Operation::SetInfo:
        return (flags & mc::info_rename) != 0 ? "rename" : "setinfo";
    case Operation::Delete:
        return "delete";
    case Operation::Format:
        return "format";
    case Operation::Unformat:
        return "unformat";
    }
    return "?";
}

class McservServer final : public IopRpcServer {
  public:
    explicit McservServer(const IopServiceDeps& deps) : deps_(deps) {}

    std::uint32_t sid() const noexcept override { return sid_mcserv; }
    std::string_view name() const noexcept override { return "mcserv"; }
    std::uint32_t receive_buffer_size() const noexcept override { return name_bytes; }
    void reset() override { reported_ = {}; }

    RpcReply call(IopCallContext& call, std::span<const std::uint8_t> args) override {
        const std::optional<Operation> op = decode_operation(call.fno);
        if (!op) {
            unsupported(call, name(),
                        "function " + hex(call.fno) + ", which this game's libmc was not seen to send");
        }
        if (*op == Operation::Init) {
            return {words({0u, mcserv_version, mcman_version}), {}, 0};
        }
        const bool named = uses_name_parameter(*op);
        if (args.size() < (named ? name_bytes : desc_bytes)) {
            unsupported(call, name(),
                        "a " + std::string(operation_name(*op, 0)) + " request of " +
                            std::to_string(args.size()) + " bytes");
        }

        RpcReply reply;
        std::uint64_t moved = 0; // bytes the card transfers, for the reply time
        Report report;
        const std::int32_t result = named ? path_call(call, *op, args, reply, moved, report)
                                          : handle_call(call, *op, args, reply, moved, report);
        reply.data = words({static_cast<std::uint32_t>(result)});
        reply.ready_at = call.machine.clock().now() + call.machine.cost().mc_reply(moved);
        if (*op != Operation::GetInfo) {
            call.machine.emit(MachineEvent{"mc_op", {}}
                                  .add("op", std::string(report.op))
                                  .add("port", std::uint64_t{report.port})
                                  .add("path", report.path)
                                  .add("result", std::int64_t{result}));
        }
        return reply;
    }

  private:
    struct Report {
        std::string_view op;
        std::uint32_t port = 0;
        std::string path;
    };

    static std::string hex(std::uint32_t v) {
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string out = "0x";
        bool started = false;
        for (int shift = 28; shift >= 0; shift -= 4) {
            const unsigned d = (v >> shift) & 0xFu;
            if (d != 0 || started || shift == 0) {
                out.push_back(digits[d]);
                started = true;
            }
        }
        return out;
    }

    IMemoryCardStore* store(std::uint32_t port) const { return port < 2 ? deps_.cards[port] : nullptr; }

    RtcDateTime now(IopCallContext& call) const {
        return deps_.rtc != nullptr ? deps_.rtc->at(call.machine.clock().now())
                                    : VirtualRtc().at(call.machine.clock().now());
    }

    // Handles are global across the ports: port * max_handles + the card's own handle.
    static std::int32_t global_fd(std::uint32_t port, std::int32_t fd) {
        return fd < 0 ? fd : static_cast<std::int32_t>(port * mc::max_handles) + fd;
    }

    std::vector<std::uint8_t> ee_bytes(IopCallContext& call, std::uint32_t address, std::size_t size) const {
        std::vector<std::uint8_t> out(size);
        if (size != 0) {
            call.machine.memory().read_bytes(address, out);
        }
        return out;
    }

    std::int32_t handle_call(IopCallContext& call, Operation op, std::span<const std::uint8_t> args,
                             RpcReply& reply, std::uint64_t& moved, Report& report) {
        report.op = operation_name(op, 0);
        if (op == Operation::GetInfo || op == Operation::Format || op == Operation::Unformat) {
            const std::uint32_t port = arg32(args, desc_port);
            report.port = port;
            IMemoryCardStore* card = store(port);
            if (op == Operation::GetInfo) {
                return get_info(call, port, card, arg32(args, desc_param), reply);
            }
            if (card == nullptr) {
                return mc::result_no_card;
            }
            return op == Operation::Format ? card->card().format(now(call)) : card->card().unformat();
        }

        // Handle calls: the handle names the port.
        const auto fd = static_cast<std::int32_t>(arg32(args, desc_fd));
        const std::uint32_t port = fd >= 0 ? static_cast<std::uint32_t>(fd) / mc::max_handles : 0;
        const std::int32_t local = fd >= 0 ? fd % static_cast<std::int32_t>(mc::max_handles) : -1;
        report.port = port;
        IMemoryCardStore* card = store(port);
        if (op == Operation::Read && arg32(args, desc_param) != 0) {
            reply.extra.push_back(
                EeWrite{arg32(args, desc_param), std::vector<std::uint8_t>(fixup_header, 0)});
        }
        if (card == nullptr || fd < 0) {
            return mc::result_no_entry;
        }
        mc::MemoryCard& mcard = card->card();
        report.path = mcard.handle_path(local);
        switch (op) {
        case Operation::Close:
            return mcard.close(local, now(call));
        case Operation::Flush:
            return mcard.flush(local, now(call));
        case Operation::Seek: {
            const std::uint32_t origin = arg32(args, desc_origin);
            if (origin > 2) {
                unsupported(call, name(), "a seek with origin " + std::to_string(origin));
            }
            return mcard.seek(local, static_cast<std::int32_t>(arg32(args, desc_offset)), origin);
        }
        case Operation::Read: {
            std::vector<std::uint8_t> data;
            const std::int32_t n = mcard.read(local, arg32(args, desc_size), data);
            if (n > 0) {
                moved = data.size();
                reply.extra.push_back(EeWrite{arg32(args, desc_buffer), std::move(data)});
            }
            return n;
        }
        case Operation::Write: {
            const std::uint32_t head = arg32(args, desc_origin);
            if (head > desc_inline_max) {
                unsupported(call, name(), "a write with " + std::to_string(head) + " inline bytes");
            }
            std::vector<std::uint8_t> data(args.begin() + desc_inline, args.begin() + desc_inline + head);
            const std::vector<std::uint8_t> body =
                ee_bytes(call, arg32(args, desc_buffer), arg32(args, desc_size));
            data.insert(data.end(), body.begin(), body.end());
            moved = data.size();
            return mcard.write(local, data);
        }
        default:
            unsupported(call, name(), "an unexpected handle call");
        }
    }

    std::int32_t get_info(IopCallContext& call, std::uint32_t port, IMemoryCardStore* card,
                          std::uint32_t info_address, RpcReply& reply) {
        mc::MemoryCard::Info info;
        if (card != nullptr) {
            info = card->card().get_info();
        } else {
            info.result = mc::result_no_card;
            info.type = 0;
        }
        if (info_address != 0) {
            std::vector<std::uint8_t> block(info_block, 0);
            put32(block, info_type, static_cast<std::uint32_t>(info.type));
            put32(block, info_free, static_cast<std::uint32_t>(info.free_clusters));
            put32(block, info_format, static_cast<std::uint32_t>(info.format));
            reply.extra.push_back(EeWrite{info_address, std::move(block)});
        }
        // One event per port whenever what GetInfo reports changes (the first time included).
        const std::uint8_t state = card == nullptr ? 1 : (info.format != 0 ? 3 : 2);
        if (port < reported_.size() && reported_[port] != state) {
            reported_[port] = state;
            call.machine.emit(MachineEvent{"mc_info", {}}
                                  .add("port", std::uint64_t{port})
                                  .add("present", card != nullptr)
                                  .add("formatted", info.format != 0)
                                  .add("free", std::int64_t{info.free_clusters}));
        }
        return info.result;
    }

    std::int32_t path_call(IopCallContext& call, Operation op, std::span<const std::uint8_t> args,
                           RpcReply& reply, std::uint64_t& moved, Report& report) {
        const std::uint32_t port = arg32(args, name_port);
        const std::uint32_t flags = arg32(args, name_flags);
        const std::string path = arg_string(args, name_path, name_path_max);
        report.op = operation_name(op, flags);
        report.port = port;
        report.path = path;
        IMemoryCardStore* card = store(port);
        if (card == nullptr) {
            return mc::result_no_card;
        }
        mc::MemoryCard& mcard = card->card();
        switch (op) {
        case Operation::Open: {
            if ((flags & ~known_open_flags) != 0) {
                unsupported(call, name(), "an open with flags " + hex(flags));
            }
            return global_fd(port, mcard.open(path, flags, now(call)));
        }
        case Operation::Chdir:
            if (arg32(args, name_table) != 0) {
                unsupported(call, name(), "a chdir that asks for the current directory");
            }
            return mcard.chdir(path);
        case Operation::GetDir: {
            if (flags != 0) {
                unsupported(call, name(), "a getdir continuation (mode " + std::to_string(flags) + ")");
            }
            const auto maxent = static_cast<std::int32_t>(arg32(args, name_maxent));
            std::vector<mc::DirEntry> entries;
            const std::int32_t n =
                mcard.get_dir(path, maxent > 0 ? static_cast<std::uint32_t>(maxent) : 0u, entries);
            if (n > 0) {
                std::vector<std::uint8_t> table;
                table.reserve(entries.size() * mc::dir_entry_size);
                for (const mc::DirEntry& e : entries) {
                    const auto bytes = mc::encode_dir_entry(e);
                    table.insert(table.end(), bytes.begin(), bytes.end());
                }
                moved = table.size();
                reply.extra.push_back(EeWrite{arg32(args, name_table), std::move(table)});
            }
            return n;
        }
        case Operation::SetInfo: {
            if ((flags & ~known_info_flags) != 0) {
                unsupported(call, name(), "a setinfo with flags " + hex(flags));
            }
            const std::vector<std::uint8_t> record =
                ee_bytes(call, arg32(args, name_table), mc::dir_entry_size);
            return mcard.set_info(path, mc::decode_dir_entry(record), flags);
        }
        case Operation::Delete:
            return mcard.remove(path, now(call));
        default:
            unsupported(call, name(), "an unexpected path call");
        }
    }

    const IopServiceDeps& deps_;
    std::array<std::uint8_t, 2> reported_{}; // per port: 0 not yet, 1 no card, 2 unformatted, 3 formatted
};

} // namespace

std::unique_ptr<IopRpcServer> make_mcserv_v1(const IopServiceDeps& deps) {
    return std::make_unique<McservServer>(deps);
}

} // namespace dq8::rt::iop_services
