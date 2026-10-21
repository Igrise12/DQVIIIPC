// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_memory.h"
#include "runtime/io/disc_source.h"
#include "runtime/iop/iop_modules.h"
#include "runtime/iop/services/services.h"
#include "runtime/iop/sif_protocol.h"
#include "runtime/machine/cost_model.h"
#include "runtime/machine/machine_services.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The file I/O server (server id 0x80000001) over the disc, in the asynchronous form the game's file library
// uses ("Protocol notes", fileio). A setup call gives the EE addresses of two completion records and checks a
// protocol version word. Every later call only queues the request: the RPC reply says "accepted", and the
// result comes afterwards as a completion record written into EE memory followed by system command
// 0x80000011, whose option word says which record. Only the cdrom0: device exists. A read writes its bytes
// straight into the EE buffer, so the record's unaligned head and tail counts are zero.
namespace dq8::rt::iop_services {

namespace {

constexpr std::uint32_t fn_open = 0x00;
constexpr std::uint32_t fn_close = 0x01;
constexpr std::uint32_t fn_read = 0x02;
constexpr std::uint32_t fn_lseek = 0x04;
constexpr std::uint32_t fn_setup = 0xFF;

// The setup call: the two record addresses in; the version word and the "option word picks the record" mode
// out.
constexpr std::size_t setup_record0 = 0x0;
constexpr std::size_t setup_record1 = 0x4;
constexpr std::uint32_t protocol_version = 0x30303033u;
constexpr std::uint32_t records_by_option = 2;

// Every request starts with what the completion echoes: the EE semaphore, the EE result pointer, the result
// size.
constexpr std::size_t req_semaphore = 0x00;
constexpr std::size_t req_result_ptr = 0x04;
constexpr std::size_t req_result_size = 0x08;
constexpr std::size_t open_flags = 0x0C;
constexpr std::size_t open_path = 0x14;
constexpr std::size_t open_path_max = 0x400;
constexpr std::size_t req_fd = 0x0C; // close, read, lseek: the IOP fd
constexpr std::size_t read_buffer = 0x10;
constexpr std::size_t read_size = 0x14;
constexpr std::size_t lseek_offset = 0x10;
constexpr std::size_t lseek_whence = 0x14;

// The completion record (the EE keeps 0x440 bytes per record; we write the words the EE reads).
constexpr std::size_t rec_semaphore = 0x00;
constexpr std::size_t rec_fno = 0x04;
constexpr std::size_t rec_result_ptr = 0x08;
constexpr std::size_t rec_result_size = 0x0C;
constexpr std::size_t rec_result = 0x10;
constexpr std::size_t rec_head_count = 0x14; // read only: unaligned head and tail byte counts, then their EE
constexpr std::size_t rec_tail_count = 0x18; // destinations; the bytes would follow at +0x24 and +0x64
constexpr std::size_t rec_read_size = 0x24;
constexpr std::size_t rec_size = 0x14;

constexpr std::int32_t err_noent = 2;
constexpr std::int32_t err_badf = 9;
constexpr std::int32_t err_nodev = 19;
constexpr std::int32_t err_inval = 22;
constexpr std::int32_t err_mfile = 24;

constexpr std::int32_t first_fd = 3;
constexpr std::int32_t max_open = 32;

class FileioServer final : public IopRpcServer {
  public:
    FileioServer(const IDiscSource* disc, const IopServiceDeps* deps) : disc_(disc), deps_(deps) {}

    std::uint32_t sid() const noexcept override { return sid_fileio; }
    std::string_view name() const noexcept override { return "fileio"; }
    std::uint32_t receive_buffer_size() const noexcept override { return 0x440; }

    RpcReply call(IopCallContext& call, std::span<const std::uint8_t> args) override {
        if (call.fno == fn_setup) {
            records_[0] = arg32(args, setup_record0);
            records_[1] = arg32(args, setup_record1);
            set_up_ = true;
            return {words({protocol_version, records_by_option}), {}, 0};
        }
        if (!set_up_) {
            unsupported(call, name(), "a request before the setup call gave the completion records");
        }
        std::vector<std::uint8_t> record(rec_size, 0);
        Cycles work = 0;
        std::vector<EeWrite> data;
        switch (call.fno) {
        case fn_open:
            put32(record, rec_result, static_cast<std::uint32_t>(open(args)));
            break;
        case fn_close: {
            const bool closed = files_.erase(static_cast<std::int32_t>(arg32(args, req_fd))) != 0;
            put32(record, rec_result, static_cast<std::uint32_t>(closed ? 0 : -err_badf));
            break;
        }
        case fn_read: {
            record.resize(rec_read_size, 0);
            const std::int32_t n = read(call, args, data);
            put32(record, rec_result, static_cast<std::uint32_t>(n));
            put32(record, rec_head_count, 0);
            put32(record, rec_tail_count, 0);
            work = call.machine.cost().cdvd_read(n > 0 ? static_cast<std::uint64_t>(n) : 0);
            break;
        }
        case fn_lseek:
            put32(record, rec_result, static_cast<std::uint32_t>(lseek(args)));
            break;
        default:
            unsupported(call, name(), "not implemented");
        }
        put32(record, rec_semaphore, arg32(args, req_semaphore));
        put32(record, rec_fno, call.fno);
        put32(record, rec_result_ptr, arg32(args, req_result_ptr));
        put32(record, rec_result_size, arg32(args, req_result_size));

        // The completion goes after the "accepted" reply, once the work is done.
        const std::uint32_t slot = next_record_++ & 1u;
        data.push_back(EeWrite{records_[slot], std::move(record)});
        const Cycles accepted = call.machine.clock().now() + call.machine.cost().rpc_reply(4);
        deps_->send_command(sif::cid_fileio_done, slot, {}, std::move(data),
                            accepted + work + call.machine.cost().rpc_reply(rec_read_size));
        return {words({1}), {}, 0};
    }

  private:
    struct OpenFile {
        DiscFile file;
        std::uint64_t position = 0;
    };

    std::int32_t open(std::span<const std::uint8_t> args) {
        std::string device;
        const std::string full = arg_string(args, open_path, open_path_max);
        const std::string_view path = split_device(full, device);
        if (device != "cdrom0" && device != "cdrom") {
            return -err_nodev;
        }
        const auto file = disc_ != nullptr ? disc_->find(path) : std::nullopt;
        if (!file) {
            return -err_noent;
        }
        static_cast<void>(arg32(args, open_flags)); // read-only device: every mode reads
        std::int32_t fd = first_fd;
        while (files_.count(fd) != 0) {
            ++fd;
        }
        if (fd >= first_fd + max_open) {
            return -err_mfile;
        }
        files_.emplace(fd, OpenFile{*file, 0});
        return fd;
    }

    // Reads into `data` (one write into the EE buffer); returns the byte count or a negative error.
    std::int32_t read(IopCallContext& call, std::span<const std::uint8_t> args, std::vector<EeWrite>& data) {
        const auto it = files_.find(static_cast<std::int32_t>(arg32(args, req_fd)));
        if (it == files_.end()) {
            return -err_badf;
        }
        const auto size = static_cast<std::int32_t>(arg32(args, read_size));
        if (size < 0) {
            return -err_inval;
        }
        OpenFile& f = it->second;
        const std::uint64_t left = f.file.size > f.position ? f.file.size - f.position : 0;
        const auto n =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(left, static_cast<std::uint32_t>(size)));
        if (n != 0) {
            std::vector<std::uint8_t> bytes(n);
            const std::uint64_t first = f.position / IDiscSource::sector_size;
            const std::uint64_t last = (f.position + n - 1) / IDiscSource::sector_size;
            const auto count = static_cast<std::uint32_t>(last - first + 1);
            std::vector<std::uint8_t> sectors(static_cast<std::size_t>(count) * IDiscSource::sector_size);
            const std::uint32_t lsn = f.file.lsn + static_cast<std::uint32_t>(first);
            if (call.machine.tracing(TraceCategory::rpc)) {
                call.machine.trace(TraceCategory::rpc, "disc read lsn " + std::to_string(lsn) + " sectors " +
                                                           std::to_string(count) + " (fileio)");
            }
            disc_->read_sectors(lsn, count, sectors);
            const std::size_t skip = f.position % IDiscSource::sector_size;
            std::copy_n(sectors.begin() + static_cast<std::ptrdiff_t>(skip), n, bytes.begin());
            data.push_back(EeWrite{arg32(args, read_buffer), std::move(bytes)});
        }
        f.position += n;
        return static_cast<std::int32_t>(n);
    }

    std::int32_t lseek(std::span<const std::uint8_t> args) {
        const auto it = files_.find(static_cast<std::int32_t>(arg32(args, req_fd)));
        if (it == files_.end()) {
            return -err_badf;
        }
        const auto offset = static_cast<std::int32_t>(arg32(args, lseek_offset));
        std::int64_t base = 0;
        switch (arg32(args, lseek_whence)) {
        case 0:
            base = 0;
            break;
        case 1:
            base = static_cast<std::int64_t>(it->second.position);
            break;
        case 2:
            base = static_cast<std::int64_t>(it->second.file.size);
            break;
        default:
            return -err_inval;
        }
        const std::int64_t position = base + offset;
        if (position < 0 || position > 0x7FFFFFFF) {
            return -err_inval;
        }
        it->second.position = static_cast<std::uint64_t>(position);
        return static_cast<std::int32_t>(position);
    }

    const IDiscSource* disc_;
    const IopServiceDeps* deps_;
    std::map<std::int32_t, OpenFile> files_;
    std::uint32_t records_[2] = {0, 0};
    bool set_up_ = false;
    std::uint32_t next_record_ = 0;
};

} // namespace

std::unique_ptr<IopRpcServer> make_fileio(const IDiscSource* disc, const IopServiceDeps& deps) {
    return std::make_unique<FileioServer>(disc, &deps);
}

} // namespace dq8::rt::iop_services
