// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// SPDX-FileCopyrightText: PCSX2 Dev Team
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors: the DMA chain walker
// (writeIORegister's channel start), tryProcessScratchpadDma and completeDmacChannel of
// ps2xRuntime/src/lib/ps2_memory.cpp, reworked for deferred completion, destination chains and interleave.
// DMA address resolution follows PCSX2 (GPL-3.0-or-later) dmaGetAddr/SPRdmaGetAddr (pcsx2/Dmac.cpp,
// 5b6e6e2); D_ENABLE's power-on value is PCSX2's (pcsx2/Hw.cpp).
#include "runtime/dma/dmac.h"

#include "dq8/abi/ps2recomp/ps2_address.h"
#include "runtime/dma/gif.h"
#include "runtime/dma/vif.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/machine/cost_model.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

namespace dq8::rt {

namespace {

// Channel register blocks, relative to Dmac::base, by channel number.
constexpr std::array<std::uint32_t, dma_channel_count> channel_offset = {
    0x0000, 0x1000, 0x2000, 0x3000, 0x3400, 0x4000, 0x4400, 0x4800, 0x5000, 0x5400,
};
constexpr std::uint32_t reg_chcr = 0x00;
constexpr std::uint32_t reg_madr = 0x10;
constexpr std::uint32_t reg_qwc = 0x20;
constexpr std::uint32_t reg_tadr = 0x30;
constexpr std::uint32_t reg_asr0 = 0x40;
constexpr std::uint32_t reg_asr1 = 0x50;
constexpr std::uint32_t reg_sadr = 0x80;

constexpr std::uint32_t ctrl_offset = 0x1000E000u - Dmac::base;
constexpr std::uint32_t enabler_offset = 0x1000F520u - Dmac::base;
constexpr std::uint32_t enablew_offset = 0x1000F590u - Dmac::base;
constexpr std::uint32_t enable_power_on = 0x1201u; // pcsx2/Hw.cpp

constexpr unsigned ch_vif0 = 0, ch_vif1 = 1, ch_gif = 2, ch_ipu_from = 3, ch_ipu_to = 4, ch_sif0 = 5,
                   ch_sif1 = 6, ch_sif2 = 7, ch_spr_from = 8, ch_spr_to = 9;

struct DmaTag {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    std::uint32_t qwc() const noexcept { return static_cast<std::uint32_t>(lo & 0xFFFFu); }
    std::uint32_t id() const noexcept { return static_cast<std::uint32_t>((lo >> 28) & 7u); }
    bool irq() const noexcept { return ((lo >> 31) & 1u) != 0; }
    std::uint32_t addr() const noexcept { return static_cast<std::uint32_t>(lo >> 32) & 0xFFFFFFF0u; }
    std::uint32_t upper16() const noexcept { return static_cast<std::uint32_t>((lo >> 16) & 0xFFFFu); }
};

} // namespace

void Dmac::attach(MachineServices& machine) {
    machine_ = &machine;
    chan_window_.emplace(*this, 0u);
    ctrl_window_.emplace(*this, ctrl_offset);
    enabler_window_.emplace(*this, enabler_offset);
    enablew_window_.emplace(*this, enablew_offset);
    machine.mmio().map(base, 0x5500u, *chan_window_);
    machine.mmio().map(0x1000E000u, 0x70u, *ctrl_window_);
    machine.mmio().map(0x1000F520u, 0x10u, *enabler_window_);
    machine.mmio().map(0x1000F590u, 0x10u, *enablew_window_);
    vif0_.set_drained_callback([this] { release(ch_vif0); });
    vif1_.set_drained_callback([this] { release(ch_vif1); });
    gif_.set_path3_drained_callback([this] { release(ch_gif); });
}

void Dmac::reset() {
    for (Channel& c : channels_) {
        if (c.completion != 0) {
            machine_->events().cancel(c.completion);
        }
        c = Channel{};
    }
    stats_ = {};
    // DMAE set: the state the BIOS leaves the DMAC in before it starts a game (there is no BIOS here).
    ctrl_ = ctrl_dmae;
    pcr_ = 0;
    sqwc_ = 0;
    rbsr_ = 0;
    rbor_ = 0;
    stadr_ = 0;
    enable_ = enable_power_on;
}

std::optional<unsigned> Dmac::channel_at(std::uint32_t offset) noexcept {
    for (unsigned ch = 0; ch < dma_channel_count; ++ch) {
        if (offset >= channel_offset[ch] && offset < channel_offset[ch] + 0x100u) {
            return ch;
        }
    }
    return std::nullopt;
}

bool Dmac::read32(std::uint32_t offset, std::uint32_t& value, const MmioContext& at) {
    if (const auto ch = channel_at(offset)) {
        const Channel& c = channels_[*ch];
        switch (offset - channel_offset[*ch]) {
        case reg_chcr:
            value = c.chcr;
            machine_->note_poll(at.pc, base + offset, value);
            return true;
        case reg_madr:
            value = c.madr;
            return true;
        case reg_qwc:
            value = c.qwc;
            return true;
        case reg_tadr:
            value = c.tadr;
            return true;
        case reg_asr0:
            value = c.asr0;
            return true;
        case reg_asr1:
            value = c.asr1;
            return true;
        case reg_sadr:
            // Only the scratchpad channels have a SADR; the others read 0 (see the write side).
            value = (*ch == ch_spr_from || *ch == ch_spr_to) ? c.sadr : 0u;
            return true;
        default:
            return false;
        }
    }
    switch (offset) {
    case ctrl_offset:
        value = ctrl_;
        return true;
    case ctrl_offset + 0x10:
        value = machine_->interrupts().dmac_stat();
        machine_->note_poll(at.pc, base + offset, value);
        return true;
    case ctrl_offset + 0x20:
        value = pcr_;
        return true;
    case ctrl_offset + 0x30:
        value = sqwc_;
        return true;
    case ctrl_offset + 0x40:
        value = rbsr_;
        return true;
    case ctrl_offset + 0x50:
        value = rbor_;
        return true;
    case ctrl_offset + 0x60:
        value = stadr_;
        return true;
    case enabler_offset:
        value = enable_;
        machine_->note_poll(at.pc, base + offset, value);
        return true;
    default:
        return false;
    }
}

bool Dmac::write32(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, const MmioContext& /*at*/) {
    if (const auto ch = channel_at(offset)) {
        Channel& c = channels_[*ch];
        switch (offset - channel_offset[*ch]) {
        case reg_chcr: {
            const std::uint32_t chcr = merge_masked(c.chcr, value, mask);
            const bool str_before = (c.chcr & chcr_str) != 0;
            c.chcr = chcr;
            if ((chcr & chcr_str) == 0 && str_before) {
                // Stopping a channel: whatever has been delivered stays delivered; no completion.
                if (c.completion != 0) {
                    machine_->events().cancel(c.completion);
                    c.completion = 0;
                }
                c.busy = c.waiting = c.held = false;
            } else if ((chcr & chcr_str) != 0 && !c.busy) {
                start(*ch);
            }
            break;
        }
        case reg_madr:
            c.madr = merge_masked(c.madr, value, mask);
            break;
        case reg_qwc:
            c.qwc = merge_masked(c.qwc, value, mask) & 0xFFFFu;
            break;
        case reg_tadr:
            c.tadr = merge_masked(c.tadr, value, mask);
            break;
        case reg_asr0:
            c.asr0 = merge_masked(c.asr0, value, mask);
            break;
        case reg_asr1:
            c.asr1 = merge_masked(c.asr1, value, mask);
            break;
        case reg_sadr:
            // The SDK's DMA reset clears SADR on channels 0-4 too; those writes are accepted and dropped.
            if (*ch == ch_spr_from || *ch == ch_spr_to) {
                c.sadr = merge_masked(c.sadr, value, mask) & 0x3FF0u;
            }
            break;
        default:
            return false;
        }
        machine_->note_activity();
        return true;
    }
    switch (offset) {
    case ctrl_offset:
        ctrl_ = merge_masked(ctrl_, value, mask);
        kick_waiting();
        break;
    case ctrl_offset + 0x10:
        // D_STAT: status bits clear and mask bits toggle on the 1s written.
        machine_->interrupts().write_dmac_stat(value & mask);
        break;
    case ctrl_offset + 0x20:
        pcr_ = merge_masked(pcr_, value, mask);
        break;
    case ctrl_offset + 0x30:
        sqwc_ = merge_masked(sqwc_, value, mask);
        break;
    case ctrl_offset + 0x40:
        rbsr_ = merge_masked(rbsr_, value, mask);
        break;
    case ctrl_offset + 0x50:
        rbor_ = merge_masked(rbor_, value, mask);
        break;
    case ctrl_offset + 0x60:
        stadr_ = merge_masked(stadr_, value, mask);
        break;
    case enablew_offset:
        enable_ = merge_masked(enable_, value, mask);
        kick_waiting();
        break;
    default:
        return false;
    }
    machine_->note_activity();
    return true;
}

void Dmac::kick_waiting() {
    for (unsigned ch = 0; ch < dma_channel_count; ++ch) {
        if (channels_[ch].waiting) {
            start(ch);
        }
    }
}

void Dmac::fail(unsigned ch, const std::string& what) {
    StopInfo info;
    info.reason = StopReason::unsupported;
    info.address = base + channel_offset[ch];
    info.message = "DMA channel " + std::to_string(ch) + " (" +
                   std::string(to_string(static_cast<DmaChannel>(ch))) + "): " + what + " (CHCR " +
                   hex32(channels_[ch].chcr) + ")";
    machine_->stop(std::move(info));
#if defined(__GNUC__)
    __builtin_unreachable(); // stop() is [[noreturn]]; GCC does not carry that through the virtual call
#endif
}

void Dmac::start(unsigned ch) {
    Channel& c = channels_[ch];
    // SIF0/SIF1 transfers are the IOP link's (session C): the registers are stored, nothing moves here.
    if (ch == ch_sif0 || ch == ch_sif1) {
        return;
    }
    if ((ctrl_ & ctrl_dmae) == 0 || (enable_ & enable_cpnd) != 0) {
        c.waiting = true;
        return;
    }
    c.waiting = false;
    const std::uint32_t mode = (c.chcr >> 2) & 3u;
    Run run;
    switch (ch) {
    case ch_vif0:
    case ch_gif:
        run = mode == 0 ? run_normal(ch) : mode == 1 ? run_source_chain(ch) : Run{};
        if (mode > 1) {
            fail(ch, "mode " + std::to_string(mode) + " is not valid for this channel");
        }
        break;
    case ch_vif1:
        if ((c.chcr & chcr_dir) == 0) {
            run = run_vif1_to_memory(ch);
        } else if (mode <= 1) {
            run = mode == 0 ? run_normal(ch) : run_source_chain(ch);
        } else {
            fail(ch, "mode " + std::to_string(mode) + " is not valid for this channel");
        }
        break;
    case ch_spr_from:
        if (((ctrl_ >> 2) & 3u) != 0) {
            fail(ch, "the MFIFO (D_CTRL.MFD) is not modelled");
        }
        run = run_spr_from(ch);
        break;
    case ch_spr_to:
        run = run_spr_to(ch);
        break;
    case ch_ipu_from:
    case ch_ipu_to:
        fail(ch, "the IPU is not modelled in M3");
    case ch_sif2:
    default:
        fail(ch, "SIF2 transfers are not modelled");
    }
    Stats& s = stats_[ch];
    ++s.transfers;
    s.qwords += run.qwords;
    s.tags += run.tags;
    if (machine_->tracing(TraceCategory::dma)) {
        machine_->trace(TraceCategory::dma, std::string(to_string(static_cast<DmaChannel>(ch))) + " mode " +
                                                std::to_string(mode) + " chcr " + hex32(c.chcr) + " tadr " +
                                                hex32(c.tadr) + ": " + std::to_string(run.qwords) +
                                                " qwords, " + std::to_string(run.tags) + " tags");
    }

    c.busy = true;
    const Cycles duration = std::max<Cycles>(1, (run.qwords + run.tags) * machine_->cost().dma_per_qword);
    c.finish_at = machine_->clock().now() + duration;
    c.held = target_holding(ch);
    if (!c.held) {
        c.completion = machine_->events().schedule(c.finish_at, "dma-complete", [this, ch](Cycles) {
            channels_[ch].completion = 0;
            complete(ch);
        });
    }
    machine_->note_activity();
}

bool Dmac::target_holding(unsigned ch) const {
    switch (ch) {
    case ch_vif0:
        return vif0_.holding_input();
    case ch_vif1:
        return vif1_.holding_input();
    case ch_gif:
        return gif_.path3_queued();
    default:
        return false;
    }
}

void Dmac::release(unsigned ch) {
    Channel& c = channels_[ch];
    if (!c.busy || !c.held || target_holding(ch)) {
        return;
    }
    c.held = false;
    const Cycles at = std::max(c.finish_at, machine_->clock().now() + 1);
    c.completion = machine_->events().schedule(at, "dma-complete", [this, ch](Cycles) {
        channels_[ch].completion = 0;
        complete(ch);
    });
}

void Dmac::complete(unsigned ch) {
    Channel& c = channels_[ch];
    c.busy = false;
    c.chcr &= ~chcr_str;
    machine_->interrupts().raise_dmac(static_cast<DmaChannel>(ch));
    machine_->note_activity();
}

std::uint8_t* Dmac::qword_ptr(std::uint32_t addr, unsigned ch) {
    GuestMemory& mem = machine_->memory();
    if ((addr & 0x80000000u) != 0 || (addr & 0x70000000u) == 0x70000000u) {
        return mem.scratchpad() + (addr & 0x3FF0u);
    }
    const std::uint32_t phys = addr & 0x1FFFFFF0u;
    if (phys < PS2_RAM_SIZE) {
        return mem.rdram() + phys;
    }
    fail(ch, "address " + hex32(addr) + " is not RAM or the scratchpad");
}

void Dmac::read_qwords(std::uint32_t addr, std::uint32_t qwc, std::vector<std::uint8_t>& out, unsigned ch) {
    out.resize(std::size_t{qwc} * 16u);
    for (std::uint32_t i = 0; i < qwc; ++i) {
        std::memcpy(out.data() + std::size_t{i} * 16u, qword_ptr(addr + i * 16u, ch), 16);
    }
}

void Dmac::write_qwords(std::uint32_t addr, std::span<const std::uint8_t> bytes, unsigned ch) {
    const std::size_t qwc = bytes.size() / 16;
    for (std::size_t i = 0; i < qwc; ++i) {
        std::memcpy(qword_ptr(addr + static_cast<std::uint32_t>(i * 16u), ch), bytes.data() + i * 16u, 16);
    }
}

void Dmac::deliver(unsigned ch, std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) {
        return;
    }
    switch (ch) {
    case ch_vif0:
        vif0_.push(bytes);
        break;
    case ch_vif1:
        vif1_.push(bytes);
        break;
    case ch_gif:
        gif_.path_data(gs::GifPath::path3, bytes);
        break;
    case ch_spr_to: {
        Channel& c = channels_[ch];
        std::uint8_t* spr = machine_->memory().scratchpad();
        for (std::size_t i = 0; i + 16 <= bytes.size(); i += 16) {
            std::memcpy(spr + c.sadr, bytes.data() + i, 16);
            c.sadr = (c.sadr + 16u) & 0x3FF0u;
        }
        break;
    }
    default:
        break;
    }
}

Dmac::Run Dmac::run_normal(unsigned ch) {
    Channel& c = channels_[ch];
    Run run;
    read_qwords(c.madr, c.qwc, buffer_, ch);
    c.madr += c.qwc * 16u;
    run.qwords = c.qwc;
    c.qwc = 0;
    deliver(ch, buffer_);
    return run;
}

Dmac::Run Dmac::run_source_chain(unsigned ch) {
    Channel& c = channels_[ch];
    Run run;
    std::uint32_t asp = (c.chcr >> 4) & 3u;
    const bool tte = (c.chcr & chcr_tte) != 0;
    const bool tie = (c.chcr & chcr_tie) != 0;

    // A transfer left over from the previous tag (QWC > 0) goes first; if that tag ended the chain
    // (refe, end, or an IRQ with TIE), the channel stops after it.
    if (c.qwc > 0) {
        read_qwords(c.madr, c.qwc, buffer_, ch);
        c.madr += c.qwc * 16u;
        run.qwords += c.qwc;
        c.qwc = 0;
        deliver(ch, buffer_);
        const std::uint32_t last_id = (c.chcr >> 28) & 7u;
        const bool last_irq = ((c.chcr >> 31) & 1u) != 0;
        if (last_id == 0 || last_id == 7 || (last_irq && tie)) {
            return run;
        }
    }

    for (;;) {
        if (run.tags >= max_tags) {
            fail(ch, "chain longer than " + std::to_string(max_tags) + " tags from TADR " + hex32(c.tadr));
        }
        std::array<std::uint8_t, 16> tp{};
        std::memcpy(tp.data(), qword_ptr(c.tadr, ch), 16);
        DmaTag tag;
        std::memcpy(&tag.lo, tp.data(), 8);
        std::memcpy(&tag.hi, tp.data() + 8, 8);
        ++run.tags;
        c.chcr = (c.chcr & 0x0000FFFFu) | (tag.upper16() << 16);
        const std::uint32_t qwc = tag.qwc();
        bool end = false;
        std::uint32_t data_addr = 0;
        const std::uint32_t tag_addr = c.tadr;
        switch (tag.id()) {
        case 0: // refe: data at ADDR, then end
            data_addr = tag.addr();
            c.tadr += 16u;
            end = true;
            break;
        case 1: // cnt: data follows the tag, the next tag follows the data
            data_addr = c.tadr + 16u;
            c.tadr = data_addr + qwc * 16u;
            break;
        case 2: // next: data follows the tag, the next tag is at ADDR
            data_addr = c.tadr + 16u;
            c.tadr = tag.addr();
            break;
        case 3: // ref
        case 4: // refs (stall control is not modelled: transfers are synchronous)
            data_addr = tag.addr();
            c.tadr += 16u;
            break;
        case 5: { // call: push the address after the data, continue at ADDR
            data_addr = c.tadr + 16u;
            const std::uint32_t ret = data_addr + qwc * 16u;
            if (asp == 0) {
                c.asr0 = ret;
            } else if (asp == 1) {
                c.asr1 = ret;
            } else {
                fail(ch, "call tag with a full address stack (ASP 2) at " + hex32(tag_addr));
            }
            ++asp;
            c.tadr = tag.addr();
            break;
        }
        case 6: // ret: pop the address stack, or end when it is empty
            data_addr = c.tadr + 16u;
            if (asp == 2) {
                c.tadr = c.asr1;
                asp = 1;
            } else if (asp == 1) {
                c.tadr = c.asr0;
                asp = 0;
            } else {
                c.tadr = data_addr + qwc * 16u;
                end = true;
            }
            break;
        default: // 7 end: data follows the tag, then end
            data_addr = c.tadr + 16u;
            c.tadr = data_addr + qwc * 16u;
            end = true;
            break;
        }
        if (tte) {
            if (ch == ch_vif0 || ch == ch_vif1) {
                // The tag's upper 64 bits go to the VIF ahead of the data (two VIFcodes).
                deliver(ch, std::span<const std::uint8_t>(tp.data() + 8, 8));
            } else if (ch == ch_spr_to) {
                deliver(ch, std::span<const std::uint8_t>(tp.data(), 16));
            }
        }
        read_qwords(data_addr, qwc, buffer_, ch);
        c.madr = data_addr + qwc * 16u;
        run.qwords += qwc;
        deliver(ch, buffer_);
        if (end || (tag.irq() && tie)) {
            break;
        }
    }
    c.qwc = 0;
    c.chcr = (c.chcr & ~(3u << 4)) | ((asp & 3u) << 4);
    return run;
}

Dmac::Run Dmac::run_vif1_to_memory(unsigned ch) {
    Channel& c = channels_[ch];
    if (((c.chcr >> 2) & 3u) != 0) {
        fail(ch, "VIF1 -> memory transfers must use normal mode");
    }
    Run run;
    buffer_.assign(std::size_t{c.qwc} * 16u, 0);
    gif_.read_back(buffer_);
    write_qwords(c.madr, buffer_, ch);
    c.madr += c.qwc * 16u;
    run.qwords = c.qwc;
    c.qwc = 0;
    return run;
}

Dmac::Run Dmac::run_spr_from(unsigned ch) {
    Channel& c = channels_[ch];
    const std::uint32_t mode = (c.chcr >> 2) & 3u;
    std::uint8_t* spr = machine_->memory().scratchpad();
    Run run;
    auto copy_out = [&](std::uint32_t qwc) {
        for (std::uint32_t i = 0; i < qwc; ++i) {
            std::memcpy(qword_ptr(c.madr, ch), spr + c.sadr, 16);
            c.madr += 16u;
            c.sadr = (c.sadr + 16u) & 0x3FF0u;
        }
        run.qwords += qwc;
    };
    if (mode == 0) {
        copy_out(c.qwc);
        c.qwc = 0;
        return run;
    }
    if (mode == 2) { // interleave: TQWC quadwords out, then skip SQWC in memory
        const std::uint32_t tqwc = (sqwc_ >> 16) & 0xFFu;
        const std::uint32_t skip = sqwc_ & 0xFFu;
        if (tqwc == 0) {
            fail(ch, "interleave with D_SQWC.TQWC = 0");
        }
        while (c.qwc > 0) {
            const std::uint32_t n = std::min(tqwc, c.qwc);
            copy_out(n);
            c.qwc -= n;
            if (c.qwc > 0) {
                c.madr += skip * 16u;
            }
        }
        return run;
    }
    if (mode != 1) {
        fail(ch, "mode 3 is not valid");
    }
    // Destination chain: tags come from the scratchpad stream; cnts and cnt set the address, end stops.
    const bool tie = (c.chcr & chcr_tie) != 0;
    if (c.qwc > 0) {
        copy_out(c.qwc);
        c.qwc = 0;
    }
    for (;;) {
        if (run.tags >= max_tags) {
            fail(ch, "destination chain longer than " + std::to_string(max_tags) + " tags");
        }
        DmaTag tag;
        std::memcpy(&tag.lo, spr + c.sadr, 8);
        std::memcpy(&tag.hi, spr + c.sadr + 8, 8);
        c.sadr = (c.sadr + 16u) & 0x3FF0u;
        ++run.tags;
        c.chcr = (c.chcr & 0x0000FFFFu) | (tag.upper16() << 16);
        const std::uint32_t id = tag.id();
        if (id != 0 && id != 1 && id != 7) {
            fail(ch, "destination tag id " + std::to_string(id) + " (only cnts, cnt and end exist)");
        }
        c.madr = tag.addr();
        copy_out(tag.qwc());
        if (id == 7 || (tag.irq() && tie)) {
            break;
        }
    }
    return run;
}

Dmac::Run Dmac::run_spr_to(unsigned ch) {
    Channel& c = channels_[ch];
    const std::uint32_t mode = (c.chcr >> 2) & 3u;
    if (mode == 1) {
        return run_source_chain(ch);
    }
    Run run;
    if (mode == 0) {
        return run_normal(ch);
    }
    if (mode != 2) {
        fail(ch, "mode 3 is not valid");
    }
    // Interleave: TQWC quadwords in, then skip SQWC in memory.
    const std::uint32_t tqwc = (sqwc_ >> 16) & 0xFFu;
    const std::uint32_t skip = sqwc_ & 0xFFu;
    if (tqwc == 0) {
        fail(ch, "interleave with D_SQWC.TQWC = 0");
    }
    while (c.qwc > 0) {
        const std::uint32_t n = std::min(tqwc, c.qwc);
        read_qwords(c.madr, n, buffer_, ch);
        deliver(ch, buffer_);
        c.madr += n * 16u;
        run.qwords += n;
        c.qwc -= n;
        if (c.qwc > 0) {
            c.madr += skip * 16u;
        }
    }
    return run;
}

} // namespace dq8::rt
