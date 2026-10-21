// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors: ps2xIOP/src/modules/mcserv.cpp.
// Changes: the service runs on IopRpcServer (arguments arrive as the call's bytes, replies and the EE-side
// info block are returned instead of written through a host interface); the parameter structs are offset
// tables; the host memory-card backend is replaced by a fixed card model (M3: port 0 holds a present,
// unformatted card, port 1 is empty); the init reply carries the versions this game's library requires.
#include "runtime/iop/services/services.h"
#include "runtime/machine/machine_services.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// The memory-card server (server id 0x80000400; "Protocol notes", mcserv). Every call but init is answered
// with one result word; GetInfo also fills the EE's info block (card type, free clusters, format state) and a
// read clears the EE's unaligned-copy counts. Saves are out of scope in M3: a request that would need a
// formatted card gets the "unformatted" result, and formatting stops the guest.
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
    GetEnt,
    ChangePriority,
    Unknown,
};

enum class Flavor {
    OldMcserv,
    NewXmcserv,
};

// The function numbers of both library generations; this game uses the first table (init 0xFE).
Operation decode_operation(std::uint32_t function, Flavor& flavor) {
    flavor = Flavor::NewXmcserv;
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
    case 0x12u:
        return Operation::GetEnt;
    case 0x14u:
        return Operation::ChangePriority;
    default:
        break;
    }

    flavor = Flavor::OldMcserv;
    switch (function) {
    case 0x70u:
        return Operation::Init;
    case 0x71u:
        return Operation::Open;
    case 0x72u:
        return Operation::Close;
    case 0x73u:
        return Operation::Read;
    case 0x74u:
        return Operation::Write;
    case 0x75u:
        return Operation::Seek;
    case 0x76u:
        return Operation::GetDir;
    case 0x77u:
        return Operation::Format;
    case 0x78u:
        return Operation::GetInfo;
    case 0x79u:
        return Operation::Delete;
    case 0x7Au:
        return Operation::Flush;
    case 0x7Bu:
        return Operation::Chdir;
    case 0x7Cu:
        return Operation::SetInfo;
    case 0x80u:
        return Operation::Unformat;
    default:
        return Operation::Unknown;
    }
}

bool uses_name_parameter(Operation operation) {
    return operation == Operation::Open || operation == Operation::Chdir || operation == Operation::GetDir ||
           operation == Operation::SetInfo || operation == Operation::Delete ||
           operation == Operation::GetEnt;
}

// Descriptor requests (fd-based and port-based calls).
constexpr std::size_t desc_port = 0x04;
constexpr std::size_t desc_param = 0x1C; // EE address of the info block (GetInfo) or the read fix-up block
constexpr std::size_t desc_size = 0x30;
// Name requests (path-based calls).
constexpr std::size_t name_port = 0x00;
constexpr std::size_t name_size = 0x414;

// The EE info block GetInfo fills: type at +0, free clusters at +4, format state at +0x90.
constexpr std::size_t info_type = 0x00;
constexpr std::size_t info_free = 0x04;
constexpr std::size_t info_format = 0x90;
constexpr std::size_t info_block_old = 64;
constexpr std::size_t info_block_new = 192;
// A read's fix-up block starts with the two unaligned byte counts.
constexpr std::size_t fixup_counts = 16;

constexpr std::int32_t succeeded = 0;
constexpr std::int32_t no_format = -2;
constexpr std::int32_t no_card = -12;
constexpr std::int32_t denied = -5;
// The init reply's version words: this game's library needs mcserv >= 0x20A and mcman >= 0x20E.
constexpr std::uint32_t mcserv_version = 0x020Au;
constexpr std::uint32_t mcman_version = 0x020Eu;
constexpr std::int32_t card_type_ps2 = 2;

class McservServer final : public IopRpcServer {
  public:
    std::uint32_t sid() const noexcept override { return sid_mcserv; }
    std::string_view name() const noexcept override { return "mcserv"; }
    std::uint32_t receive_buffer_size() const noexcept override { return name_size; }

    RpcReply call(IopCallContext& call, std::span<const std::uint8_t> args) override {
        Flavor flavor = Flavor::NewXmcserv;
        const Operation operation = decode_operation(call.fno, flavor);
        if (operation == Operation::Init) {
            if (flavor == Flavor::NewXmcserv) {
                return {words({static_cast<std::uint32_t>(succeeded), mcserv_version, mcman_version}), {}, 0};
            }
            return {words({static_cast<std::uint32_t>(succeeded)}), {}, 0};
        }
        if (operation == Operation::Unknown) {
            unsupported(call, name(), "unknown function");
        }

        RpcReply reply;
        std::int32_t result = denied;
        if (uses_name_parameter(operation)) {
            if (args.size() >= name_size) {
                result = card_present(arg32(args, name_port)) ? no_format : no_card;
            }
        } else if (args.size() >= desc_size) {
            result = descriptor_operation(call, operation, flavor, args, reply);
        }
        reply.data = words({static_cast<std::uint32_t>(result)});
        return reply;
    }

  private:
    // M3's card model: port 0 holds a card that is present and unformatted; port 1 is empty. The slot is
    // ignored (the game's save code passes slot 1).
    static bool card_present(std::uint32_t port) { return port == 0; }

    std::int32_t descriptor_operation(IopCallContext& call, Operation operation, Flavor flavor,
                                      std::span<const std::uint8_t> args, RpcReply& reply) {
        const std::uint32_t port = arg32(args, desc_port);
        const std::uint32_t param = arg32(args, desc_param);
        switch (operation) {
        case Operation::GetInfo: {
            const bool present = card_present(port);
            if (param != 0) {
                std::vector<std::uint8_t> info(flavor == Flavor::NewXmcserv ? info_block_new : info_block_old,
                                               0);
                put32(info, info_type, static_cast<std::uint32_t>(present ? card_type_ps2 : 0));
                put32(info, info_free, 0);
                if (flavor == Flavor::NewXmcserv) {
                    put32(info, info_format, 0);
                }
                reply.extra.push_back(EeWrite{param, std::move(info)});
            }
            if (reported_.insert(port).second) {
                call.machine.emit(MachineEvent{"mc_info", {}}
                                      .add("port", std::uint64_t{port})
                                      .add("present", present)
                                      .add("formatted", false));
            }
            return present ? no_format : no_card;
        }
        case Operation::Read:
            if (param != 0) {
                reply.extra.push_back(EeWrite{param, std::vector<std::uint8_t>(fixup_counts, 0)});
            }
            return denied; // no file can be open on an unformatted card
        case Operation::Close:
        case Operation::Seek:
        case Operation::Write:
        case Operation::Flush:
            return denied;
        case Operation::Unformat:
            return card_present(port) ? no_format : no_card;
        case Operation::ChangePriority:
            return succeeded;
        case Operation::Format:
            unsupported(call, name(), "formatting a memory card (saves are out of scope in M3)");
        default:
            return denied;
        }
    }

    std::set<std::uint32_t> reported_; // ports whose first GetInfo was reported
};

} // namespace

std::unique_ptr<IopRpcServer> make_mcserv_v1() {
    return std::make_unique<McservServer>();
}

} // namespace dq8::rt::iop_services
