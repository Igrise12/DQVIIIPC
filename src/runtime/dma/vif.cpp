// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// SPDX-FileCopyrightText: PCSX2 Dev Team
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp and the VIF register cases of
// ps2xRuntime/src/lib/ps2_memory.cpp. UNPACK details (V2/V3/V4-5 lanes, the MODE register, mask selectors,
// data sizes) follow PCSX2 (GPL-3.0-or-later) pcsx2/Vif_Unpack.cpp at 5b6e6e2.
#include "runtime/dma/vif.h"

#include "runtime/dma/gif.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/machine/interrupts.h"
#include "runtime/vu/vu_units.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace dq8::rt {

namespace {

constexpr std::uint32_t vif0_regs = 0x10003800u;
constexpr std::uint32_t vif1_regs = 0x10003C00u;
constexpr std::uint32_t vif0_fifo = 0x10004000u;
constexpr std::uint32_t vif1_fifo = 0x10005000u;

// Bytes of source data per vector for UNPACK's low nibble (vn << 2 | vl); 0 = no such format
// (PCSX2 nVifT).
constexpr std::array<std::uint32_t, 16> unpack_bytes = {4, 2, 1, 0, 8, 4, 2, 0, 12, 6, 3, 0, 16, 8, 4, 2};

enum Command : std::uint32_t {
    cmd_nop = 0x00,
    cmd_stcycl = 0x01,
    cmd_offset = 0x02,
    cmd_base = 0x03,
    cmd_itop = 0x04,
    cmd_stmod = 0x05,
    cmd_mskpath3 = 0x06,
    cmd_mark = 0x07,
    cmd_flushe = 0x10,
    cmd_flush = 0x11,
    cmd_flusha = 0x13,
    cmd_mscal = 0x14,
    cmd_mscalf = 0x15,
    cmd_mscnt = 0x17,
    cmd_stmask = 0x20,
    cmd_strow = 0x30,
    cmd_stcol = 0x31,
    cmd_mpg = 0x4A,
    cmd_direct = 0x50,
    cmd_directhl = 0x51,
};

std::uint32_t word_at(const std::uint8_t* p) noexcept {
    std::uint32_t w = 0;
    std::memcpy(&w, p, 4);
    return w;
}

} // namespace

std::uint32_t Vif::regs_offset() const noexcept {
    return unit_ == 0 ? vif0_regs : vif1_regs;
}

void Vif::attach(MachineServices& machine) {
    machine_ = &machine;
    const std::uint32_t regs = regs_offset();
    const std::uint32_t fifo = unit_ == 0 ? vif0_fifo : vif1_fifo;
    regs_window_.emplace(*this, 0u);
    fifo_window_.emplace(*this, fifo - regs);
    machine.mmio().map(regs, 0x180u, *regs_window_);
    machine.mmio().map(fifo, 0x10u, *fifo_window_);
}

void Vif::reset() {
    regs_ = Registers{};
    in_.clear();
    in_pos_ = 0;
    phase_ = Phase::command;
    words_needed_ = 0;
    data_.clear();
    processing_ = false;
    held_ = false;
    stop_requested_ = false;
}

void Vif::fail(const std::string& what) {
    StopInfo info;
    info.reason = StopReason::unsupported;
    info.message = std::string(name()) + ": " + what + " (VIFcode " + hex32(regs_.code) + ")";
    machine_->stop(std::move(info));
#if defined(__GNUC__)
    __builtin_unreachable(); // stop() is [[noreturn]]; GCC does not carry that through the virtual call
#endif
}

bool Vif::read32(std::uint32_t offset, std::uint32_t& value, const MmioContext& at) {
    switch (offset) {
    case reg_stat: {
        const std::size_t held_qwords = (in_.size() - in_pos_ + 15) / 16;
        const std::size_t fqc = std::min<std::size_t>(held_qwords, unit_ == 0 ? 8u : 16u);
        value = regs_.stat | (phase_ != Phase::command ? stat_vps_waiting : 0u) |
                (static_cast<std::uint32_t>(fqc) << 24);
        machine_->note_poll(at.pc, regs_offset() + offset, value);
        return true;
    }
    case reg_fbrst:
        value = 0;
        return true;
    case reg_err:
        value = regs_.err;
        return true;
    case reg_mark:
        value = regs_.mark;
        return true;
    case reg_cycle:
        value = regs_.cycle;
        return true;
    case reg_mode:
        value = regs_.mode;
        return true;
    case reg_num:
        value = regs_.num;
        return true;
    case reg_mask:
        value = regs_.mask;
        return true;
    case reg_code:
        value = regs_.code;
        return true;
    case reg_itops:
        value = regs_.itops;
        return true;
    case reg_itop:
        value = regs_.itop;
        return true;
    default:
        break;
    }
    if (unit_ == 1) {
        switch (offset) {
        case reg_base:
            value = regs_.base;
            return true;
        case reg_ofst:
            value = regs_.ofst;
            return true;
        case reg_tops:
            value = regs_.tops;
            return true;
        case reg_top:
            value = regs_.top;
            return true;
        default:
            break;
        }
    }
    if (offset >= reg_r0 && offset < reg_r0 + 0x40 && (offset & 0xFu) == 0) {
        value = regs_.row[(offset - reg_r0) / 0x10];
        return true;
    }
    if (offset >= reg_c0 && offset < reg_c0 + 0x40 && (offset & 0xFu) == 0) {
        value = regs_.col[(offset - reg_c0) / 0x10];
        return true;
    }
    return false;
}

bool Vif::write32(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, const MmioContext& /*at*/) {
    const std::uint32_t v = value & mask;
    switch (offset) {
    case reg_stat:
        if (unit_ == 1) { // only the FIFO direction is writable
            regs_.stat = (regs_.stat & ~stat_fdr) | (merge_masked(regs_.stat, value, mask) & stat_fdr);
        }
        break;
    case reg_fbrst:
        if ((v & 1u) != 0) { // RST: drops kept input too, which releases a DMA channel waiting on it
            const bool was_holding = held_;
            reset();
            if (unit_ == 1 && gif_ != nullptr) {
                gif_->set_vif_path3_mask(false);
            }
            if (was_holding && drained_) {
                drained_();
            }
        }
        if ((v & 2u) != 0) { // FBK
            regs_.stat |= stat_vfs;
        }
        if ((v & 4u) != 0) { // STP: stop at the end of the current command
            if (phase_ == Phase::command) {
                regs_.stat |= stat_vss;
            } else {
                stop_requested_ = true;
            }
        }
        if ((v & 8u) != 0) { // STC: clear the stalls and errors, then go on with kept input
            regs_.stat &= ~(stat_stalls | stat_int | stat_er0 | stat_er1);
            stop_requested_ = false;
            process();
        }
        break;
    case reg_err:
        regs_.err = merge_masked(regs_.err, value, mask) & 7u;
        break;
    case reg_mark:
        regs_.mark = merge_masked(regs_.mark, value, mask) & 0xFFFFu;
        regs_.stat &= ~stat_mrk;
        break;
    case reg_cycle:
        regs_.cycle = merge_masked(regs_.cycle, value, mask) & 0xFFFFu;
        break;
    case reg_mode:
        regs_.mode = merge_masked(regs_.mode, value, mask) & 3u;
        break;
    case reg_mask:
        regs_.mask = merge_masked(regs_.mask, value, mask);
        break;
    case reg_itops:
        regs_.itops = merge_masked(regs_.itops, value, mask) & 0x3FFu;
        break;
    case reg_itop:
        regs_.itop = merge_masked(regs_.itop, value, mask) & 0x3FFu;
        break;
    case reg_num:
    case reg_code:
        break; // read-only
    default:
        if (unit_ == 1 && offset == reg_base) {
            regs_.base = merge_masked(regs_.base, value, mask) & 0x3FFu;
        } else if (unit_ == 1 && offset == reg_ofst) {
            regs_.ofst = merge_masked(regs_.ofst, value, mask) & 0x3FFu;
        } else if (unit_ == 1 && offset == reg_tops) {
            regs_.tops = merge_masked(regs_.tops, value, mask) & 0x3FFu;
        } else if (unit_ == 1 && offset == reg_top) {
            regs_.top = merge_masked(regs_.top, value, mask) & 0x3FFu;
        } else if (offset >= reg_r0 && offset < reg_r0 + 0x40 && (offset & 0xFu) == 0) {
            auto& r = regs_.row[(offset - reg_r0) / 0x10];
            r = merge_masked(r, value, mask);
        } else if (offset >= reg_c0 && offset < reg_c0 + 0x40 && (offset & 0xFu) == 0) {
            auto& c = regs_.col[(offset - reg_c0) / 0x10];
            c = merge_masked(c, value, mask);
        } else {
            return false;
        }
        break;
    }
    machine_->note_activity();
    return true;
}

bool Vif::read128(std::uint32_t offset, std::uint8_t* out, const MmioContext& /*at*/) {
    const std::uint32_t fifo = (unit_ == 0 ? vif0_fifo : vif1_fifo) - regs_offset();
    if (offset != fifo || unit_ != 1 || gif_ == nullptr) {
        return false;
    }
    // VIF1 FIFO reads return GS -> host transfer data.
    std::memset(out, 0, 16);
    gif_->read_back(std::span<std::uint8_t>(out, 16));
    return true;
}

bool Vif::write128(std::uint32_t offset, const std::uint8_t* in, const MmioContext& /*at*/) {
    const std::uint32_t fifo = (unit_ == 0 ? vif0_fifo : vif1_fifo) - regs_offset();
    if (offset != fifo) {
        return false;
    }
    push(std::span<const std::uint8_t>(in, 16));
    machine_->note_activity();
    return true;
}

void Vif::push(std::span<const std::uint8_t> bytes) {
    in_.insert(in_.end(), bytes.begin(), bytes.end());
    process();
}

void Vif::process() {
    if (processing_) {
        return;
    }
    processing_ = true;
    while (!stalled() && in_.size() - in_pos_ >= 4) {
        if (phase_ == Phase::command) {
            regs_.code = word_at(in_.data() + in_pos_);
            in_pos_ += 4;
            words_needed_ = begin_command();
            if (words_needed_ == 0 && phase_ == Phase::command) {
                finish_command();
            } else if (words_needed_ == 0) {
                execute_data();
                finish_command();
            }
            continue;
        }
        const std::size_t available = (in_.size() - in_pos_) / 4;
        const std::size_t take = std::min<std::size_t>(words_needed_, available);
        data_.insert(data_.end(), in_.begin() + static_cast<std::ptrdiff_t>(in_pos_),
                     in_.begin() + static_cast<std::ptrdiff_t>(in_pos_ + take * 4));
        in_pos_ += take * 4;
        words_needed_ -= static_cast<std::uint32_t>(take);
        if (words_needed_ == 0) {
            execute_data();
            finish_command();
        }
    }
    if (in_pos_ == in_.size()) {
        in_.clear();
        in_pos_ = 0;
    } else if (in_pos_ > (std::size_t{1} << 16)) {
        in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(in_pos_));
        in_pos_ = 0;
    }
    processing_ = false;
    if (holding_input() && stalled()) {
        held_ = true;
    } else if (held_ && !holding_input()) {
        held_ = false;
        if (drained_) {
            drained_();
        }
    }
}

std::uint32_t Vif::begin_command() {
    const std::uint32_t code = regs_.code;
    const std::uint32_t cmd = (code >> 24) & 0x7Fu;
    const std::uint32_t imm = code & 0xFFFFu;
    const std::uint32_t num = (code >> 16) & 0xFFu;
    regs_.num = num;
    data_.clear();
    phase_ = Phase::command;
    const bool vif1 = unit_ == 1;

    if ((cmd & 0x60u) == 0x60u) { // UNPACK
        const std::uint32_t gsize = unpack_bytes[cmd & 0xFu];
        if (gsize == 0) {
            if ((regs_.err & err_me1) == 0) {
                fail("UNPACK with an invalid format");
            }
            return 0;
        }
        const std::uint32_t n_writes = num == 0 ? 256u : num;
        const auto [cl, wl] = cycle_lengths();
        const std::uint32_t vectors =
            wl <= cl ? n_writes : cl * (n_writes / wl) + std::min(n_writes % wl, cl);
        phase_ = Phase::unpack;
        return (vectors * gsize + 3) / 4;
    }
    switch (cmd) {
    case cmd_nop:
    case cmd_flushe:
        return 0;
    case cmd_stcycl:
        regs_.cycle = imm;
        return 0;
    case cmd_itop:
        regs_.itops = imm & 0x3FFu;
        return 0;
    case cmd_stmod:
        regs_.mode = imm & 3u;
        return 0;
    case cmd_mark:
        regs_.mark = imm;
        regs_.stat |= stat_mrk;
        return 0;
    case cmd_mscal:
        start_micro(false, imm * 8u);
        return 0;
    case cmd_mscnt:
        start_micro(true, 0);
        return 0;
    case cmd_stmask:
        phase_ = Phase::stmask;
        return 1;
    case cmd_strow:
        phase_ = Phase::strow;
        return 4;
    case cmd_stcol:
        phase_ = Phase::stcol;
        return 4;
    case cmd_mpg:
        phase_ = Phase::mpg;
        return (num == 0 ? 256u : num) * 2u;
    default:
        break;
    }
    if (vif1) {
        switch (cmd) {
        case cmd_offset:
            regs_.ofst = imm & 0x3FFu;
            regs_.stat &= ~stat_dbf;
            regs_.tops = regs_.base;
            return 0;
        case cmd_base:
            regs_.base = imm & 0x3FFu;
            return 0;
        case cmd_mskpath3:
            if (gif_ != nullptr) {
                gif_->set_vif_path3_mask((imm & 0x8000u) != 0);
            }
            return 0;
        case cmd_flush:
        case cmd_flusha:
            return 0; // VU and GIF work is done by the time the next command runs
        case cmd_mscalf:
            start_micro(false, imm * 8u);
            return 0;
        case cmd_direct:
        case cmd_directhl:
            phase_ = Phase::direct;
            return (imm == 0 ? 65536u : imm) * 4u;
        default:
            break;
        }
    }
    if ((regs_.err & err_me1) == 0) {
        fail("invalid VIFcode");
    }
    return 0; // masked invalid code: ignored
}

void Vif::execute_data() {
    const std::uint32_t code = regs_.code;
    switch (phase_) {
    case Phase::stmask:
        regs_.mask = word_at(data_.data());
        break;
    case Phase::strow:
        for (unsigned i = 0; i < 4; ++i) {
            regs_.row[i] = word_at(data_.data() + 4 * i);
        }
        break;
    case Phase::stcol:
        for (unsigned i = 0; i < 4; ++i) {
            regs_.col[i] = word_at(data_.data() + 4 * i);
        }
        break;
    case Phase::mpg:
        vu_.write_micro(unit_, (code & 0xFFFFu) * 8u, data_);
        break;
    case Phase::direct:
        if (gif_ != nullptr) {
            gif_->path_data(gs::GifPath::path2, data_);
        }
        break;
    case Phase::unpack:
        unpack();
        break;
    case Phase::command:
        break;
    }
    data_.clear();
}

void Vif::finish_command() {
    phase_ = Phase::command;
    if ((regs_.code & 0x80000000u) != 0 && (regs_.err & err_mii) == 0) {
        regs_.stat |= stat_int | stat_vis;
        machine_->interrupts().raise_intc(unit_ == 0 ? IntcLine::vif0 : IntcLine::vif1);
    }
    if (stop_requested_) {
        stop_requested_ = false;
        regs_.stat |= stat_vss;
    }
}

std::pair<std::uint32_t, std::uint32_t> Vif::cycle_lengths() const noexcept {
    // CYCLE as never written (both 0) unpacks as a normal write, as upstream PS2Recomp treats it; otherwise
    // WL = 0 means 256 (PCSX2's vifUnpackSetup).
    if (regs_.cycle == 0) {
        return {1u, 1u};
    }
    const std::uint32_t cl = regs_.cycle & 0xFFu;
    const std::uint32_t wl = (regs_.cycle >> 8) & 0xFFu;
    return {cl, wl == 0 ? 256u : wl};
}

void Vif::start_micro(bool continue_program, std::uint32_t start_pc) {
    std::uint32_t top = 0;
    if (unit_ == 1) {
        top = regs_.tops & 0x3FFu;
        regs_.top = top;
        if ((regs_.stat & stat_dbf) != 0) {
            regs_.tops = regs_.base;
            regs_.stat &= ~stat_dbf;
        } else {
            regs_.tops = (regs_.base + regs_.ofst) & 0x3FFu;
            regs_.stat |= stat_dbf;
        }
    }
    regs_.itop = regs_.itops & 0x3FFu;
    if (continue_program) {
        vu_.resume(unit_, top, regs_.itop);
    } else {
        vu_.start(unit_, start_pc, top, regs_.itop);
    }
}

void Vif::unpack() {
    const std::uint32_t code = regs_.code;
    const std::uint32_t cmd = (code >> 24) & 0x7Fu;
    const std::uint32_t imm = code & 0xFFFFu;
    const std::uint32_t vl = cmd & 3u;
    const std::uint32_t vn = (cmd >> 2) & 3u;
    const bool masked = (cmd & 0x10u) != 0;
    const bool v4_5 = vn == 3 && vl == 3;
    const bool usn = (imm & 0x4000u) != 0;
    const std::uint32_t gsize = unpack_bytes[cmd & 0xFu];
    const std::uint32_t n_writes = regs_.num == 0 ? 256u : regs_.num;
    const auto [cl, wl] = cycle_lengths();
    const bool filling = wl > cl;
    const std::uint32_t mode = v4_5 ? 0u : regs_.mode;

    std::span<std::uint8_t> mem = vu_.data(unit_);
    const std::uint32_t qword_mask = static_cast<std::uint32_t>(mem.size() / 16) - 1u;
    std::uint32_t dest = imm & 0x3FFu;
    if (unit_ == 1 && (imm & 0x8000u) != 0) {
        dest += regs_.tops;
    }
    dest &= qword_mask;

    // Component k of source vector at byte `at`, sign- or zero-extended.
    auto component = [&](std::size_t at, unsigned k) -> std::uint32_t {
        switch (vl) {
        case 0: {
            const std::size_t p = at + 4u * k;
            return p + 4 <= data_.size() ? word_at(data_.data() + p) : 0u;
        }
        case 1: {
            const std::size_t p = at + 2u * k;
            if (p + 2 > data_.size()) {
                return 0u;
            }
            std::uint16_t h = 0;
            std::memcpy(&h, data_.data() + p, 2);
            return usn ? h
                       : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(h)));
        }
        default: {
            const std::size_t p = at + k;
            if (p + 1 > data_.size()) {
                return 0u;
            }
            const std::uint8_t b = data_[p];
            return usn ? b
                       : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(b)));
        }
        }
    };

    std::size_t src = 0; // byte offset of the next source vector
    std::uint32_t cycle = 0;
    for (std::uint32_t w = 0; w < n_writes; ++w) {
        const bool has_data = !filling || cycle < cl;
        std::array<std::uint32_t, 4> lanes{};
        if (has_data) {
            if (v4_5) { // one halfword: 5-5-5-1 colour, each channel moved to the top of a byte
                std::uint32_t h = 0;
                if (src + 2 <= data_.size()) {
                    std::memcpy(&h, data_.data() + src, 2);
                }
                lanes = {(h & 0x001Fu) << 3, (h & 0x03E0u) >> 2, (h & 0x7C00u) >> 7, (h & 0x8000u) >> 8};
            } else if (vn == 0) { // S: one value in every lane
                const std::uint32_t s = component(src, 0);
                lanes = {s, s, s, s};
            } else if (vn == 1) { // V2: x y x y
                const std::uint32_t x = component(src, 0);
                const std::uint32_t y = component(src, 1);
                lanes = {x, y, x, y};
            } else { // V3 reads a fourth component past the vector (the next one's x, or 0); V4 all four
                lanes = {component(src, 0), component(src, 1), component(src, 2), component(src, 3)};
            }
            src += gsize;
        }
        const std::uint32_t row = std::min<std::uint32_t>(cycle, 3u);
        std::uint8_t* q = mem.data() + std::size_t{dest} * 16u;
        for (unsigned field = 0; field < 4; ++field) {
            std::uint32_t selector = masked ? (regs_.mask >> ((row * 4 + field) * 2)) & 3u : 0u;
            if (!has_data && selector == 0) {
                selector = 1; // a filling cycle writes the row where it would write data
            }
            std::uint32_t out = 0;
            switch (selector) {
            case 0:
                switch (mode) {
                case 1:
                    out = lanes[field] + regs_.row[field];
                    break;
                case 2:
                    regs_.row[field] += lanes[field];
                    out = regs_.row[field];
                    break;
                case 3:
                    regs_.row[field] = lanes[field];
                    out = lanes[field];
                    break;
                default:
                    out = lanes[field];
                    break;
                }
                break;
            case 1:
                out = regs_.row[field];
                break;
            case 2:
                out = regs_.col[row];
                break;
            default:
                continue; // write-protected
            }
            std::memcpy(q + field * 4, &out, 4);
        }
        ++cycle;
        if (cycle == wl) {
            cycle = 0;
            if (!filling) {
                dest += cl - wl; // skipping write: the block advances by CL
            }
        }
        dest = (dest + 1) & qword_mask;
    }
}

} // namespace dq8::rt
