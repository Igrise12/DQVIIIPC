// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/dma/gif.h"

#include "runtime/gs/gs_priv_device.h"
#include "runtime/gs/gs_sink.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace dq8::rt {

namespace {

constexpr std::uint32_t fifo_offset = Gif::fifo_base - Gif::regs_base;

struct GifTag {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    std::uint32_t nloop() const noexcept { return static_cast<std::uint32_t>(lo & 0x7FFFu); }
    bool eop() const noexcept { return ((lo >> 15) & 1u) != 0; }
    std::uint32_t flg() const noexcept { return static_cast<std::uint32_t>((lo >> 58) & 3u); }
    std::uint32_t nreg() const noexcept {
        const auto n = static_cast<std::uint32_t>((lo >> 60) & 0xFu);
        return n == 0 ? 16u : n;
    }
    // Payload quadwords after the tag: PACKED one per register, REGLIST two registers per quadword
    // (padded), IMAGE and the disabled format one per loop.
    std::uint64_t payload_qwords() const noexcept {
        const std::uint64_t regs = std::uint64_t{nloop()} * nreg();
        switch (flg()) {
        case 0:
            return regs;
        case 1:
            return (regs + 1) / 2;
        default:
            return nloop();
        }
    }
};

GifTag read_tag(const std::uint8_t* p) noexcept {
    GifTag t;
    std::memcpy(&t.lo, p, 8);
    std::memcpy(&t.hi, p + 8, 8);
    return t;
}

// Bits per pixel of a transfer in pixel storage format `psm` (GS register BITBLTBUF.SPSM/DPSM).
std::uint32_t transfer_bits(std::uint32_t psm) noexcept {
    switch (psm) {
    case 0x00: // PSMCT32
    case 0x30: // PSMZ32
        return 32;
    case 0x01: // PSMCT24
    case 0x31: // PSMZ24
        return 24;
    case 0x02: // PSMCT16
    case 0x0A: // PSMCT16S
    case 0x32: // PSMZ16
    case 0x3A: // PSMZ16S
        return 16;
    case 0x13: // PSMT8
    case 0x1B: // PSMT8H
        return 8;
    case 0x14: // PSMT4
    case 0x24: // PSMT4HL
    case 0x2C: // PSMT4HH
        return 4;
    default:
        return 32;
    }
}

// GS register addresses the GIF acts on.
constexpr std::uint8_t gs_bitbltbuf = 0x50;
constexpr std::uint8_t gs_trxreg = 0x52;
constexpr std::uint8_t gs_trxdir = 0x53;
constexpr std::uint8_t gs_signal = 0x60;
constexpr std::uint8_t gs_finish = 0x61;
constexpr std::uint8_t gs_label = 0x62;

} // namespace

void Gif::attach(MachineServices& machine) {
    machine_ = &machine;
    regs_window_.emplace(*this, 0u);
    fifo_window_.emplace(*this, fifo_offset);
    machine.mmio().map(regs_base, 0x100u, *regs_window_);
    machine.mmio().map(fifo_base, 0x10u, *fifo_window_);
}

void Gif::reset() {
    paths_ = {};
    stats_ = {};
    path3_queue_.clear();
    vif_mask_ = false;
    ctrl_ = 0;
    mode_ = 0;
    last_tag_ = {};
    bitbltbuf_ = 0;
    trxreg_ = 0;
    readback_pending_ = 0;
}

bool Gif::read32(std::uint32_t offset, std::uint32_t& value, const MmioContext& at) {
    switch (offset) {
    case reg_stat:
        // Transfers are delivered synchronously, so the paths are idle and the FIFO empty; what the guest
        // can see is the PATH3 mask and whether masked packets wait.
        value = (mode_ & (stat_m3r | stat_imt)) | (vif_mask_ ? stat_m3p : 0u) | (ctrl_ & stat_pse) |
                (path3_queue_.empty() ? 0u : stat_p3q);
        machine_->note_poll(at.pc, regs_base + offset, value);
        return true;
    case reg_tag0:
    case reg_tag0 + 0x10:
    case reg_tag0 + 0x20:
    case reg_tag0 + 0x30:
        value = last_tag_[(offset - reg_tag0) / 0x10];
        return true;
    case reg_cnt:
    case reg_p3cnt:
    case reg_p3tag:
        value = 0;
        return true;
    default:
        return false;
    }
}

bool Gif::write32(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, const MmioContext& /*at*/) {
    switch (offset) {
    case reg_ctrl: {
        const std::uint32_t v = value & mask;
        if ((v & 1u) != 0) { // RST: drop partial packets and queued PATH3 data (releasing a held GIF DMA)
            const bool had_queue = !path3_queue_.empty();
            paths_ = {};
            path3_queue_.clear();
            readback_pending_ = 0;
            if (had_queue && path3_drained_) {
                path3_drained_();
            }
        }
        ctrl_ = merge_masked(ctrl_, v, mask) & stat_pse;
        break;
    }
    case reg_mode:
        mode_ = merge_masked(mode_, value, mask) & (stat_m3r | stat_imt);
        drain_path3();
        break;
    default:
        return false;
    }
    machine_->note_activity();
    return true;
}

bool Gif::write128(std::uint32_t offset, const std::uint8_t* in, const MmioContext& /*at*/) {
    if (offset != fifo_offset) {
        return false;
    }
    path_data(gs::GifPath::path3, std::span<const std::uint8_t>(in, 16));
    machine_->note_activity();
    return true;
}

void Gif::path_data(gs::GifPath path, std::span<const std::uint8_t> qwords) {
    PathState& p = paths_[index(path)];
    const std::size_t count = qwords.size() / 16;
    const std::uint8_t* q = qwords.data();
    std::size_t i = 0;
    while (i < count) {
        if (p.remaining == 0) {
            const GifTag tag = read_tag(q + i * 16);
            std::memcpy(last_tag_.data(), q + i * 16, 16);
            p.packet.insert(p.packet.end(), q + i * 16, q + i * 16 + 16);
            p.remaining = tag.payload_qwords();
            p.eop = tag.eop();
            ++i;
        } else {
            const std::size_t take =
                static_cast<std::size_t>(std::min<std::uint64_t>(p.remaining, count - i));
            p.packet.insert(p.packet.end(), q + i * 16, q + (i + take) * 16);
            p.remaining -= take;
            i += take;
        }
        if (p.remaining == 0 && p.eop) {
            std::vector<std::uint8_t> packet;
            packet.swap(p.packet);
            p.eop = false;
            packet_complete(path, std::move(packet));
        }
    }
}

void Gif::packet_complete(gs::GifPath path, std::vector<std::uint8_t> packet) {
    PathStats& s = stats_[index(path)];
    ++s.packets;
    s.bytes += packet.size();
    if (path == gs::GifPath::path3 && (path3_masked() || !path3_queue_.empty())) {
        path3_queue_.push_back(std::move(packet));
        return;
    }
    deliver(path, packet);
}

void Gif::deliver(gs::GifPath path, std::span<const std::uint8_t> packet) {
    if (machine_->tracing(TraceCategory::gs)) {
        machine_->trace(TraceCategory::gs, "path" + std::to_string(static_cast<unsigned>(path)) + " packet " +
                                               std::to_string(packet.size()) + " bytes");
    }
    sink_.gif_transfer(path, packet);
    scan(packet);
}

void Gif::scan(std::span<const std::uint8_t> packet) {
    std::size_t pos = 0;
    while (pos + 16 <= packet.size()) {
        const GifTag tag = read_tag(packet.data() + pos);
        pos += 16;
        const std::uint64_t payload = tag.payload_qwords();
        if (tag.flg() == 0) {
            const std::uint32_t nreg = tag.nreg();
            for (std::uint64_t k = 0; k < payload && pos + (k + 1) * 16 <= packet.size(); ++k) {
                const auto desc = static_cast<std::uint32_t>((tag.hi >> (4 * (k % nreg))) & 0xFu);
                if (desc != 0xE) {
                    continue;
                }
                const std::uint8_t* entry = packet.data() + pos + k * 16;
                std::uint64_t data = 0;
                std::memcpy(&data, entry, 8);
                ad_write(entry[8], data);
            }
        }
        pos += payload * 16;
    }
}

void Gif::ad_write(std::uint8_t reg, std::uint64_t data) {
    switch (reg) {
    case gs_bitbltbuf:
        bitbltbuf_ = data;
        break;
    case gs_trxreg:
        trxreg_ = data;
        break;
    case gs_trxdir:
        if ((data & 3u) == 1u) { // local -> host
            const std::uint64_t width = trxreg_ & 0xFFFu;
            const std::uint64_t height = (trxreg_ >> 32) & 0xFFFu;
            const std::uint64_t bits = width * height * transfer_bits((bitbltbuf_ >> 24) & 0x3Fu);
            readback_pending_ = ((bits + 7) / 8 + 15) / 16 * 16;
        }
        break;
    case gs_signal:
        priv_.signal(data);
        break;
    case gs_finish:
        priv_.finish();
        break;
    case gs_label:
        priv_.label(data);
        break;
    default:
        break;
    }
}

void Gif::set_vif_path3_mask(bool masked) {
    vif_mask_ = masked;
    drain_path3();
}

void Gif::drain_path3() {
    if (path3_queue_.empty()) {
        return;
    }
    while (!path3_queue_.empty() && !path3_masked()) {
        std::vector<std::uint8_t> packet = std::move(path3_queue_.front());
        path3_queue_.pop_front();
        deliver(gs::GifPath::path3, packet);
    }
    if (path3_queue_.empty() && path3_drained_) {
        path3_drained_();
    }
}

std::size_t Gif::read_back(std::span<std::uint8_t> out) {
    const std::size_t n = sink_.read_transfer(out);
    readback_pending_ -= std::min<std::uint64_t>(readback_pending_, n);
    return n;
}

} // namespace dq8::rt
