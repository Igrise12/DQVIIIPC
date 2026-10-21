// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_stop.h"
#include "runtime/io/disc_source.h"
#include "runtime/iop/services/services.h"
#include "runtime/machine/cost_model.h"
#include "runtime/machine/machine_services.h"

#include <algorithm>
#include <memory>

// The CD/DVD server's six server ids ("Protocol notes", cdvdfsv): init, the S-commands (drive queries
// answered at once), the N-commands (reads and seeks, whose end packet waits for the virtual disc time),
// SearchFile, and two disc-ready servers (0x8000059A and its newer twin 0x8000059C, which the library tries
// first). The drive always holds a readable PS2 DVD; the last N-command error is kept for the S-command that
// reports it.
namespace dq8::rt::iop_services {

namespace {

constexpr std::uint32_t disk_type_ps2_dvd = 0x14;
constexpr std::uint32_t error_none = 0;
constexpr std::uint32_t ready_complete = 2;
// Init reply: two version words whose high bytes the library checks, and a word it switches on (0: none of
// its special cases).
constexpr std::uint32_t init_version = 0x0200;

constexpr std::uint32_t scmd_disk_type = 0x03;
constexpr std::uint32_t scmd_error = 0x04;
constexpr std::uint32_t scmd_tray_request = 0x05;
constexpr std::uint32_t scmd_media_mode = 0x22;
constexpr std::uint32_t scmd_thread_priority = 0x23;

constexpr std::uint32_t ncmd_read = 0x01;
constexpr std::uint32_t ncmd_seek = 0x05;
constexpr std::uint32_t ncmd_standby = 0x06;
constexpr std::uint32_t ncmd_stop = 0x07;
constexpr std::uint32_t ncmd_pause = 0x08;
constexpr std::uint32_t ncmd_disk_ready = 0x0E;

// Read request.
constexpr std::size_t read_lsn = 0x00;
constexpr std::size_t read_sectors = 0x04;
constexpr std::size_t read_buffer = 0x08;
constexpr std::size_t read_data_pattern = 0x0E; // 0: 2048-byte sectors
constexpr std::size_t read_fixup = 0x10;        // EE address of the unaligned head/tail block
constexpr std::size_t seek_lsn = 0x00;
// The fix-up block starts with the two byte counts the EE copies; zero, because every byte goes straight to
// the EE buffer.
constexpr std::size_t fixup_header_size = 0x10;

// SearchFile request: an output area, the path, the EE address the file record goes to, the layer.
constexpr std::size_t search_path = 0x024;
constexpr std::size_t search_path_max = 0x100;
constexpr std::size_t search_dest = 0x124;
// The file record: LSN, size, name, date, a flag word.
constexpr std::size_t record_lsn = 0x00;
constexpr std::size_t record_size_field = 0x04;
constexpr std::size_t record_name = 0x08;
constexpr std::size_t record_name_max = 16;
constexpr std::size_t record_size = 0x24;

struct DriveState {
    std::uint32_t last_error = error_none;
};

class CdvdServer : public IopRpcServer {
  public:
    CdvdServer(std::uint32_t sid, std::string_view name, const IDiscSource* disc,
               std::shared_ptr<DriveState> drive)
        : sid_(sid), name_(name), disc_(disc), drive_(std::move(drive)) {}
    std::uint32_t sid() const noexcept override { return sid_; }
    std::string_view name() const noexcept override { return name_; }
    std::uint32_t receive_buffer_size() const noexcept override { return 0x200; }

  protected:
    Cycles disc_time(IopCallContext& call, std::uint64_t bytes) const {
        return call.machine.clock().now() + call.machine.cost().cdvd_read(bytes);
    }

    std::uint32_t sid_;
    std::string_view name_;
    const IDiscSource* disc_;
    std::shared_ptr<DriveState> drive_;
};

class InitServer final : public CdvdServer {
  public:
    using CdvdServer::CdvdServer;
    RpcReply call(IopCallContext& call, std::span<const std::uint8_t>) override {
        if (call.fno != 0) {
            unsupported(call, name(), "not implemented");
        }
        drive_->last_error = error_none;
        return {words({1, init_version, init_version, 0}), {}, 0};
    }
};

class SCommandServer final : public CdvdServer {
  public:
    using CdvdServer::CdvdServer;
    RpcReply call(IopCallContext& call, std::span<const std::uint8_t>) override {
        switch (call.fno) {
        case scmd_disk_type:
            return {words({disk_type_ps2_dvd}), {}, 0};
        case scmd_error:
            return {words({drive_->last_error}), {}, 0};
        case scmd_tray_request:
            return {words({1, 0}), {}, 0}; // done; the tray has not moved
        case scmd_media_mode:
        case scmd_thread_priority:
            return {words({1}), {}, 0};
        default:
            unsupported(call, name(), "S-command not implemented");
        }
    }
};

class NCommandServer final : public CdvdServer {
  public:
    using CdvdServer::CdvdServer;
    RpcReply call(IopCallContext& call, std::span<const std::uint8_t> args) override {
        switch (call.fno) {
        case ncmd_read:
            return read(call, args);
        case ncmd_seek:
            if (arg32(args, seek_lsn) >= disc_->sector_count()) {
                unsupported(call, name(),
                            "seek to sector " + std::to_string(arg32(args, seek_lsn)) +
                                " past the end of the disc");
            }
            drive_->last_error = error_none;
            return {words({1}), {}, disc_time(call, 0)};
        case ncmd_standby:
        case ncmd_stop:
        case ncmd_pause:
            drive_->last_error = error_none;
            return {words({1}), {}, disc_time(call, 0)};
        case ncmd_disk_ready:
            return {words({ready_complete}), {}, 0};
        default:
            unsupported(call, name(), "N-command not implemented");
        }
    }

  private:
    // The sectors go straight into the EE buffer; the end packet waits for the virtual read time.
    RpcReply read(IopCallContext& call, std::span<const std::uint8_t> args) {
        const std::uint32_t lsn = arg32(args, read_lsn);
        const std::uint32_t count = arg32(args, read_sectors);
        const std::uint32_t buffer = arg32(args, read_buffer);
        if (args.size() > read_data_pattern && args[read_data_pattern] != 0) {
            unsupported(call, name(), "read with sector pattern " + std::to_string(args[read_data_pattern]));
        }
        if (count == 0 || std::uint64_t{lsn} + count > disc_->sector_count()) {
            unsupported(call, name(),
                        "read of " + std::to_string(count) + " sectors at " + std::to_string(lsn) +
                            " outside the disc");
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count) * IDiscSource::sector_size);
        disc_->read_sectors(lsn, count, bytes);
        drive_->last_error = error_none;
        RpcReply reply;
        reply.data = words({1});
        reply.ready_at = disc_time(call, bytes.size());
        reply.extra.push_back(EeWrite{buffer, std::move(bytes)});
        if (const std::uint32_t fixup = arg32(args, read_fixup); fixup != 0) {
            reply.extra.push_back(EeWrite{fixup, std::vector<std::uint8_t>(fixup_header_size, 0)});
        }
        return reply;
    }
};

class SearchServer final : public CdvdServer {
  public:
    using CdvdServer::CdvdServer;
    RpcReply call(IopCallContext& call, std::span<const std::uint8_t> args) override {
        if (call.fno != 0) {
            unsupported(call, name(), "not implemented");
        }
        std::string device;
        const std::string full = arg_string(args, search_path, search_path_max);
        const auto file = disc_->find(split_device(full, device));
        drive_->last_error = error_none;
        if (!file) {
            return {words({0}), {}, disc_time(call, 0)};
        }
        std::vector<std::uint8_t> record(record_size, 0);
        put32(record, record_lsn, file->lsn);
        put32(record, record_size_field, static_cast<std::uint32_t>(file->size));
        const std::string base = file->path.substr(file->path.find_last_of('/') + 1) + ";1";
        std::copy_n(base.begin(), std::min(base.size(), record_name_max - 1), record.begin() + record_name);
        RpcReply reply;
        reply.data = words({1});
        reply.ready_at = disc_time(call, 0);
        if (const std::uint32_t dest = arg32(args, search_dest); dest != 0) {
            reply.extra.push_back(EeWrite{dest, std::move(record)});
        }
        return reply;
    }
};

class ReadyServer final : public CdvdServer {
  public:
    using CdvdServer::CdvdServer;
    RpcReply call(IopCallContext& call, std::span<const std::uint8_t>) override {
        if (call.fno != 0) {
            unsupported(call, name(), "not implemented");
        }
        return {words({ready_complete}), {}, 0};
    }
};

} // namespace

std::vector<std::unique_ptr<IopRpcServer>> make_cdvdfsv(const IDiscSource* disc) {
    auto drive = std::make_shared<DriveState>();
    std::vector<std::unique_ptr<IopRpcServer>> out;
    out.push_back(std::make_unique<InitServer>(sid_cdvd_init, "cdvdfsv.init", disc, drive));
    out.push_back(std::make_unique<SCommandServer>(sid_cdvd_scmd, "cdvdfsv.scmd", disc, drive));
    out.push_back(std::make_unique<NCommandServer>(sid_cdvd_ncmd, "cdvdfsv.ncmd", disc, drive));
    out.push_back(std::make_unique<SearchServer>(sid_cdvd_search, "cdvdfsv.search", disc, drive));
    out.push_back(std::make_unique<ReadyServer>(sid_cdvd_ready, "cdvdfsv.ready", disc, drive));
    out.push_back(std::make_unique<ReadyServer>(sid_cdvd_ready2, "cdvdfsv.ready2", disc, drive));
    return out;
}

} // namespace dq8::rt::iop_services
