// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/iop/iop_server.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The native RPC servers (ADR-0011) behind the catalogue in iop_modules.h. Their request and reply layouts
// are in Docs/milestones/M3-boots.md, "Protocol notes".
namespace dq8::rt {
class IDiscSource;
class IInputSource;
class MachineServices;
struct IopServiceDeps;
} // namespace dq8::rt

namespace dq8::rt::iop_services {

// Server ids.
inline constexpr std::uint32_t sid_fileio = 0x80000001u;
inline constexpr std::uint32_t sid_iopheap = 0x80000003u;
inline constexpr std::uint32_t sid_loadfile = 0x80000006u;
inline constexpr std::uint32_t sid_cdvd_init = 0x80000592u;
inline constexpr std::uint32_t sid_cdvd_scmd = 0x80000593u;
inline constexpr std::uint32_t sid_cdvd_ncmd = 0x80000595u;
inline constexpr std::uint32_t sid_cdvd_search = 0x80000597u;
inline constexpr std::uint32_t sid_cdvd_ready = 0x8000059Au;
inline constexpr std::uint32_t sid_cdvd_ready2 = 0x8000059Cu; // the newer disc-ready server, tried first
inline constexpr std::uint32_t sid_padman = 0x80000100u;
inline constexpr std::uint32_t sid_padman_ext = 0x80000101u;
inline constexpr std::uint32_t sid_mcserv = 0x80000400u;

// `deps` must outlive the server (fileio sends its completions through it).
std::unique_ptr<IopRpcServer> make_fileio(const IDiscSource* disc, const IopServiceDeps& deps);
std::unique_ptr<IopRpcServer> make_iopheap(const IDiscSource* disc);
std::unique_ptr<IopRpcServer>
make_loadfile(std::function<std::int32_t(std::string_view path, std::span<const std::uint8_t> args)> load);
std::vector<std::unique_ptr<IopRpcServer>> make_cdvdfsv(const IDiscSource* disc);
std::vector<std::unique_ptr<IopRpcServer>> make_padman_v1(IInputSource* input);
std::unique_ptr<IopRpcServer> make_mcserv_v1();
std::unique_ptr<IopRpcServer> make_null_server(std::uint32_t sid);

// Helpers shared by the servers.

// A little-endian word of the argument block; 0 past its end.
inline std::uint32_t arg32(std::span<const std::uint8_t> args, std::size_t offset) {
    std::uint32_t v = 0;
    if (offset + 4 <= args.size()) {
        std::memcpy(&v, args.data() + offset, 4);
    }
    return v;
}

// A NUL-terminated string of at most `max` bytes at `offset`.
inline std::string arg_string(std::span<const std::uint8_t> args, std::size_t offset, std::size_t max) {
    std::string out;
    for (std::size_t i = offset; i < args.size() && i < offset + max && args[i] != 0; ++i) {
        out.push_back(static_cast<char>(args[i]));
    }
    return out;
}

// Reply words, little-endian.
inline std::vector<std::uint8_t> words(std::initializer_list<std::uint32_t> values) {
    std::vector<std::uint8_t> out(values.size() * 4);
    std::size_t i = 0;
    for (const std::uint32_t v : values) {
        std::memcpy(out.data() + 4 * i++, &v, 4);
    }
    return out;
}
// Stores a word into a reply that is already large enough (nothing past its end).
inline void put32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    if (offset + 4 <= bytes.size()) {
        std::memcpy(bytes.data() + offset, &value, 4);
    }
}

// Stops the guest: the server met a request it does not implement.
[[noreturn]] void unsupported(IopCallContext& call, std::string_view server, std::string_view what);

// Splits "cdrom0:\BIN\X.BIN;1" into its device ("cdrom0", lower case) and the rest; the device is empty when
// the path names none.
std::string_view split_device(std::string_view path, std::string& device);

} // namespace dq8::rt::iop_services
