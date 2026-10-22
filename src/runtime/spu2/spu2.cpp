// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The SPU2 device (spu2.h): the chip on the virtual clock and on the IOP's bus, DMA channels and interrupt
// line. The timing of DMA follows PCSX2 (pcsx2/SPU2/Dma.cpp, tag v2.7.524): 24 IOP cycles per 16-bit word.
#include "runtime/spu2/spu2.h"

#include "dq8/core/hash.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/iop/iop_memory.h"
#include "runtime/machine/cost_model.h"
#include "runtime/platform/audio.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dq8::rt {

namespace {

constexpr Cycles sample_cycles = ee_cycles_per_spu2_sample;
constexpr Cycles never = std::numeric_limits<Cycles>::max();
constexpr std::uint32_t dma_channel[2] = {4, 7};

std::string hex16(std::uint32_t v) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string s = "0x0000";
    for (int i = 0; i < 4; ++i) {
        s[5 - static_cast<std::size_t>(i)] = digits[(v >> (4 * i)) & 0xF];
    }
    return s;
}

void put_u64(core::Sha1& sha, std::uint64_t v) {
    std::uint8_t bytes[8];
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
    sha.update(bytes);
}

} // namespace

Spu2::Spu2(Spu2Config config) : config_(config) {}

Spu2::~Spu2() = default;

void Spu2::attach(MachineServices& machine) {
    machine_ = &machine;
}

void Spu2::connect(IIopHardwareHost& host) {
    host_ = &host;
    chip_.set_iop_ram(host.ram().data(), IopMemory::size);
}

void Spu2::reset() {
    if (event_ != 0 && machine_ != nullptr) {
        machine_->events().cancel(event_);
    }
    event_ = 0;
    event_at_ = 0;
    chip_.reset();
    started_ = false;
    mixed_ = 0;
    processed_ = 0;
    dma_ = {};
    pending_.clear();
    late_accesses_ = 0;
}

IopDeviceMapping Spu2::mapping() {
    IopDeviceMapping m;
    m.base = base;
    m.size = size;
    m.device = this;
    m.client = this;
    m.dma = this;
    m.dma_channels = {4, 7};
    return m;
}

// ---------------------------------------------------------------------------------------------------------
// The IOP side

bool Spu2::read(std::uint32_t offset, std::uint32_t width, std::uint32_t& value, const IopAccess& at) {
    if (width != 2 || (offset & 1) != 0 || offset >= size) {
        return false;
    }
    if (started_) {
        note_time(at.at);
        catch_up(at.at);
        if (event_at_ <= processed_) {
            reschedule(true);
        }
    }
    value = chip_.read(offset);
    return true;
}

bool Spu2::write(std::uint32_t offset, std::uint32_t width, std::uint32_t value, const IopAccess& at) {
    if (width != 2 || (offset & 1) != 0 || offset >= size) {
        return false;
    }
    if (!started_) {
        start(at.at);
    }
    note_time(at.at);
    catch_up(at.at);
    try {
        chip_.write(offset, static_cast<std::uint16_t>(value));
    } catch (const spu2::Unsupported& e) {
        stop_unsupported(std::string(e.what()) + " (register " + hex16(offset) + " = " + hex16(value) + ")",
                         at.pc);
    }
    if (machine_ != nullptr && machine_->tracing(TraceCategory::spu2)) {
        trace("write " + hex16(offset) + " = " + hex16(value) + " at sample " + std::to_string(mixed_));
    }
    deliver(std::max(at.at, processed_)); // a hit by a data-register write
    reschedule(false);
    return true;
}

IopDmaStart Spu2::dma_start(const IopDmaRequest& request) {
    std::uint32_t core = 0;
    if (request.channel == 7) {
        core = 1;
    } else if (request.channel != 4) {
        stop_unsupported("DMA on IOP channel " + std::to_string(request.channel), 0);
    }
    const std::uint32_t mode = request.chcr & ~0x01000000u;
    if (mode != 0x201 && mode != 0x200) {
        stop_unsupported("DMA mode CHCR " + hex32(request.chcr) + " on channel " +
                             std::to_string(request.channel) + " (only block transfers to and from memory)",
                         0);
    }
    const std::uint64_t words = std::uint64_t{request.bcr >> 16} * (request.bcr & 0xFFFFu) * 2;
    if (words > IopMemory::size / 2) {
        stop_unsupported("DMA of " + std::to_string(words) + " words (BCR " + hex32(request.bcr) + ")", 0);
    }
    const bool read = mode == 0x200;
    if (!started_) {
        start(request.at);
    }
    note_time(request.at);
    catch_up(request.at);
    const spu2::DmaPlan plan = chip_.begin_dma(core, read, request.madr, static_cast<std::uint32_t>(words));
    deliver(std::max(request.at, processed_));
    if (machine_ != nullptr && machine_->tracing(TraceCategory::spu2)) {
        trace(std::string(plan.deferred ? "AutoDMA"
                          : read        ? "DMA read"
                                        : "DMA write") +
              " core " + std::to_string(core) + ": " + std::to_string(words) + " words, IOP " +
              hex32(request.madr) + ", TSA " + hex32(chip_.core(core).tsa) + " at sample " +
              std::to_string(mixed_));
    }
    IopDmaStart start_result;
    if (plan.deferred) {
        dma_[core] = DmaTimer{true, true, never};
        start_result.deferred = true;
    } else {
        const Cycles per = machine_ != nullptr ? machine_->cost().spu2_dma_per_halfword : 0;
        dma_[core] = DmaTimer{true, false, request.at + plan.first_step * per};
        start_result.done_at = request.at + plan.total * per;
    }
    reschedule(true);
    return start_result;
}

void Spu2::dma_stop(std::uint32_t channel, Cycles at) {
    const std::uint32_t core = channel == 7 ? 1 : 0;
    if (started_) {
        note_time(at);
        catch_up(at);
    }
    chip_.stop_dma(core);
    dma_[core] = DmaTimer{};
    trace("DMA stop on channel " + std::to_string(channel));
    reschedule(true);
}

// ---------------------------------------------------------------------------------------------------------
// Time

void Spu2::advance_to(Cycles t) {
    if (!started_) {
        return;
    }
    catch_up(t);
    reschedule(true);
}

void Spu2::start(Cycles at) {
    started_ = true;
    mixed_ = at / sample_cycles;
    processed_ = at;
    // The samples before first use were silent.
    if (config_.output != nullptr && mixed_ > 0) {
        const std::vector<StereoFrame> silence(4096);
        for (std::uint64_t left = mixed_; left > 0;) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(left, silence.size()));
            config_.output->write(std::span<const StereoFrame>(silence.data(), n));
            left -= n;
        }
    }
    trace("first use at sample " + std::to_string(mixed_));
}

void Spu2::note_time(Cycles at) {
    if (at < processed_) {
        ++late_accesses_;
        trace("access at cycle " + std::to_string(at) + " after the chip reached " +
              std::to_string(processed_));
    }
}

void Spu2::catch_up(Cycles t) {
    if (!started_) {
        return;
    }
    for (;;) {
        const Cycles sample_end = (mixed_ + 1) * sample_cycles;
        Cycles dma_next = never;
        std::uint32_t dma_core = 0;
        for (std::uint32_t c = 0; c < 2; ++c) {
            if (dma_[c].busy && !dma_[c].deferred && dma_[c].next < dma_next) {
                dma_next = dma_[c].next;
                dma_core = c;
            }
        }
        if (sample_end <= t && sample_end <= dma_next) {
            mix_one();
        } else if (dma_next <= t) {
            dma_step(dma_core);
        } else {
            break;
        }
    }
    processed_ = std::max(processed_, t);
    flush();
}

void Spu2::mix_one() {
    const spu2::Stereo s = chip_.mix();
    pending_.push_back(StereoFrame{static_cast<std::int16_t>(s.left), static_cast<std::int16_t>(s.right)});
    ++mixed_;
    deliver(mixed_ * sample_cycles);
    if (pending_.size() >= 4096) {
        flush();
    }
}

void Spu2::dma_step(std::uint32_t core) {
    const Cycles at = dma_[core].next;
    const std::uint32_t units = chip_.dma_step(core);
    deliver(at);
    if (units == 0) {
        dma_[core] = DmaTimer{}; // the IOP completes the channel at the time dma_start returned
        trace("DMA done on core " + std::to_string(core));
    } else {
        const Cycles per = machine_ != nullptr ? machine_->cost().spu2_dma_per_halfword : 0;
        dma_[core].next = at + units * per;
    }
}

void Spu2::deliver(Cycles at) {
    const std::uint32_t irqs = chip_.take_irqs();
    if (irqs != 0) {
        if (machine_ != nullptr && machine_->tracing(TraceCategory::spu2)) {
            trace("IRQ core mask " + std::to_string(irqs) + " at cycle " + std::to_string(at) + " (IRQA " +
                  hex32(chip_.core(irqs & 1 ? 0 : 1).irqa) + ")");
        }
        if (host_ != nullptr) {
            host_->raise_irq(IopIrq::spu2, at);
        }
    }
    const std::uint32_t adma = chip_.take_adma_done();
    for (std::uint32_t c = 0; c < 2; ++c) {
        if ((adma & (1u << c)) != 0 && dma_[c].busy && dma_[c].deferred) {
            dma_[c] = DmaTimer{};
            trace("AutoDMA done on core " + std::to_string(c) + " at cycle " + std::to_string(at));
            if (host_ != nullptr) {
                host_->dma_complete(dma_channel[c], at);
            }
        }
    }
}

void Spu2::flush() {
    if (pending_.empty()) {
        return;
    }
    if (config_.output != nullptr) {
        config_.output->write(pending_);
    }
    pending_.clear();
}

void Spu2::reschedule(bool full) {
    if (!started_ || machine_ == nullptr) {
        return;
    }
    const std::uint32_t block = std::max<std::uint32_t>(1, machine_->cost().spu2_block_samples);
    const std::uint32_t k = chip_.samples_to_signal(block, full);
    Cycles when = (mixed_ + k) * sample_cycles;
    for (const DmaTimer& d : dma_) {
        if (d.busy && !d.deferred) {
            when = std::min(when, d.next);
        }
    }
    if (event_ != 0) {
        if (event_at_ == when) {
            return;
        }
        machine_->events().cancel(event_);
    }
    event_at_ = when;
    event_ = machine_->events().schedule(when, "spu2", [this](Cycles at) { on_event(at); });
}

void Spu2::on_event(Cycles when) {
    event_ = 0;
    catch_up(when);
    reschedule(true);
}

// ---------------------------------------------------------------------------------------------------------
// Reports

std::string Spu2::ram_sha1() const {
    core::Sha1 sha;
    const std::vector<std::uint8_t> bytes = ram_bytes_le();
    sha.update(bytes);
    return core::to_hex(sha.finish());
}

std::vector<std::uint8_t> Spu2::ram_bytes_le() const {
    const std::span<const std::uint16_t> ram = chip_.ram();
    std::vector<std::uint8_t> bytes(ram.size() * 2);
    for (std::size_t i = 0; i < ram.size(); ++i) {
        bytes[2 * i] = static_cast<std::uint8_t>(ram[i]);
        bytes[2 * i + 1] = static_cast<std::uint8_t>(ram[i] >> 8);
    }
    return bytes;
}

std::string Spu2::state_sha1() const {
    core::Sha1 sha;
    sha.update(chip_.serialize_state());
    put_u64(sha, started_ ? 1 : 0);
    put_u64(sha, mixed_);
    for (const DmaTimer& d : dma_) {
        put_u64(sha, (d.busy ? 1u : 0u) | (d.deferred ? 2u : 0u));
        put_u64(sha, d.busy && !d.deferred ? d.next : 0);
    }
    return core::to_hex(sha.finish());
}

void Spu2::trace(std::string message) const {
    if (machine_ != nullptr && machine_->tracing(TraceCategory::spu2)) {
        machine_->trace(TraceCategory::spu2, std::move(message));
    }
}

void Spu2::stop_unsupported(const std::string& what, std::uint32_t pc) const {
    StopInfo info;
    info.reason = StopReason::unsupported;
    info.message = "spu2: " + what + (pc != 0 ? ", IOP pc " + hex32(pc) : std::string());
    info.pc = pc;
    if (machine_ == nullptr) {
        throw std::logic_error(info.message);
    }
    machine_->stop(std::move(info));
    std::abort(); // stop() never returns; GCC cannot see that through the virtual call
}

} // namespace dq8::rt
