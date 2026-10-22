// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/hw/iop_hardware.h"

#include "runtime/ee/guest_stop.h"
#include "runtime/iop/iop_memory.h"
#include "runtime/machine/machine_services.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace dq8::rt {

namespace {

constexpr std::uint32_t dma_madr = 0x0;
constexpr std::uint32_t dma_bcr = 0x4;
constexpr std::uint32_t dma_chcr = 0x8;

std::uint32_t extract(std::uint32_t word, std::uint32_t physical, std::uint32_t width) {
    const std::uint32_t shift = (physical & 3u) * 8;
    const std::uint32_t mask = width == 4 ? 0xFFFFFFFFu : (1u << (width * 8)) - 1u;
    return (word >> shift) & mask;
}

std::uint32_t merge(std::uint32_t word, std::uint32_t physical, std::uint32_t width, std::uint32_t value) {
    if (width == 4) {
        return value;
    }
    const std::uint32_t shift = (physical & 3u) * 8;
    const std::uint32_t mask = ((1u << (width * 8)) - 1u) << shift;
    return (word & ~mask) | ((value << shift) & mask);
}

std::string channel_name(std::uint32_t channel) {
    return "DMA" + std::to_string(channel);
}

} // namespace

IopHardware::IopHardware(MachineServices& machine, IopMemory& ram, IIopHardwareOwner& owner,
                         std::vector<IopDeviceMapping> devices)
    : machine_(machine), ram_(ram), owner_(owner), devices_(std::move(devices)) {
    dma_device_.fill(-1);
    for (std::size_t i = 0; i < devices_.size(); ++i) {
        const IopDeviceMapping& d = devices_[i];
        if (d.device == nullptr || d.size == 0) {
            throw std::invalid_argument("an IOP device mapping has no device or an empty range");
        }
        if (d.base < 0x1F800000u || d.base + d.size < d.base) {
            throw std::invalid_argument("IOP device " + std::string(d.device->name()) + " at " +
                                        hex32(d.base) + " is not in the IOP's device space");
        }
        for (std::size_t j = 0; j < i; ++j) {
            const IopDeviceMapping& o = devices_[j];
            if (d.base < o.base + o.size && o.base < d.base + d.size) {
                throw std::invalid_argument("IOP devices " + std::string(o.device->name()) + " and " +
                                            std::string(d.device->name()) + " overlap");
            }
        }
        for (const std::uint32_t ch : d.dma_channels) {
            if (ch >= iop_dma_channels || d.dma == nullptr) {
                throw std::invalid_argument("IOP device " + std::string(d.device->name()) +
                                            " names DMA channel " + std::to_string(ch) +
                                            (d.dma == nullptr ? " without a DMA interface" : ""));
            }
            if (dma_device_[ch] >= 0) {
                throw std::invalid_argument("IOP DMA channel " + std::to_string(ch) + " is mapped twice");
            }
            dma_device_[ch] = static_cast<int>(i);
        }
    }
}

void IopHardware::connect_devices() {
    for (const IopDeviceMapping& d : devices_) {
        if (d.client != nullptr) {
            d.client->connect(*this);
        }
    }
}

void IopHardware::reset() {
    for (Channel& c : channels_) {
        if (c.event != 0) {
            machine_.events().cancel(c.event);
        }
        c = Channel{};
    }
    scratchpad_.fill(0);
    ssbus_a_.fill(0);
    ssbus_b_.fill(0);
    i_stat_ = 0;
    i_mask_ = 0;
    i_ctrl_ = 1;
    dpcr_ = 0;
    dicr_ = 0;
    dpcr2_ = 0;
    dicr2_ = 0;
    dmacen_ = 0;
    dmacinten_ = 0;
    device_accesses_ = 0;
}

Cycles IopHardware::iop_now() const {
    return owner_.hw_now();
}

const IopDeviceMapping* IopHardware::device_at(std::uint32_t physical) const {
    for (const IopDeviceMapping& d : devices_) {
        if (physical >= d.base && physical - d.base < d.size) {
            return &d;
        }
    }
    return nullptr;
}

std::optional<std::uint32_t> IopHardware::hle_slot(std::uint32_t physical) const {
    if (physical < iop_map::hle_page || physical - iop_map::hle_page >= iop_map::hle_page_size) {
        return std::nullopt;
    }
    const std::uint32_t offset = physical - iop_map::hle_page;
    if (offset % iop_map::hle_slot_size != 0 || offset / iop_map::hle_slot_size >= hle_slots_) {
        return std::nullopt;
    }
    return offset / iop_map::hle_slot_size;
}

std::optional<std::uint32_t> IopHardware::dma_register(std::uint32_t physical, std::uint32_t& channel) {
    if (physical >= iop_map::dma_low && physical < iop_map::dma_low + 7 * 0x10) {
        channel = (physical - iop_map::dma_low) / 0x10;
        return (physical - iop_map::dma_low) % 0x10;
    }
    if (physical >= iop_map::dma_high && physical < iop_map::dma_high + 7 * 0x10) {
        channel = 7 + (physical - iop_map::dma_high) / 0x10;
        return (physical - iop_map::dma_high) % 0x10;
    }
    return std::nullopt;
}

bool IopHardware::read(std::uint32_t physical, std::uint32_t width, std::uint32_t& value, std::uint32_t pc) {
    if (physical >= iop_map::scratchpad && physical - iop_map::scratchpad < iop_map::scratchpad_size) {
        std::uint32_t v = 0;
        std::memcpy(&v, scratchpad_.data() + (physical - iop_map::scratchpad), width);
        value = v;
        return true;
    }
    if (const IopDeviceMapping* d = device_at(physical)) {
        ++device_accesses_;
        const IopAccess at{owner_.hw_now(), pc};
        const std::uint32_t offset = physical - d->base;
        if (width == 4) {
            std::uint32_t lo = 0;
            std::uint32_t hi = 0;
            if (!d->device->read(offset, 2, lo, at) || !d->device->read(offset + 2, 2, hi, at)) {
                return false;
            }
            value = (lo & 0xFFFFu) | (hi << 16);
        } else if (!d->device->read(offset, width, value, at)) {
            return false;
        }
        owner_.hw_poll(pc, physical, value);
        return true;
    }
    const auto word = register_read(physical & ~3u, pc);
    if (!word) {
        return false;
    }
    value = extract(*word, physical, width);
    owner_.hw_poll(pc, physical, value);
    return true;
}

bool IopHardware::write(std::uint32_t physical, std::uint32_t width, std::uint32_t value, std::uint32_t pc) {
    if (physical >= iop_map::scratchpad && physical - iop_map::scratchpad < iop_map::scratchpad_size) {
        std::memcpy(scratchpad_.data() + (physical - iop_map::scratchpad), &value, width);
        return true;
    }
    if (const IopDeviceMapping* d = device_at(physical)) {
        ++device_accesses_;
        const IopAccess at{owner_.hw_now(), pc};
        const std::uint32_t offset = physical - d->base;
        bool ok = false;
        if (width == 4) {
            ok = d->device->write(offset, 2, value & 0xFFFFu, at) &&
                 d->device->write(offset + 2, 2, value >> 16, at);
        } else {
            ok = d->device->write(offset, width, value, at);
        }
        if (ok) {
            owner_.hw_activity();
        }
        return ok;
    }
    const std::uint32_t aligned = physical & ~3u;
    std::uint32_t channel = 0;
    const auto dma_field = dma_register(aligned, channel);
    const bool plain =
        (aligned >= iop_map::ssbus_a && aligned < iop_map::ssbus_a + 4 * iop_map::ssbus_words) ||
        (aligned >= iop_map::ssbus_b && aligned < iop_map::ssbus_b + 4 * iop_map::ssbus_words) ||
        aligned == iop_map::i_mask || aligned == iop_map::dpcr || aligned == iop_map::dpcr2 ||
        aligned == iop_map::dmacen || aligned == iop_map::dmacinten || (dma_field && *dma_field != dma_chcr);
    if (width != 4 && !plain) {
        // Registers with side effects (acknowledge bits, the transfer start) take whole words only.
        const bool known = aligned == iop_map::i_stat || aligned == iop_map::i_ctrl ||
                           aligned == iop_map::dicr || aligned == iop_map::dicr2 || dma_field.has_value();
        if (!known) {
            return false;
        }
        owner_.hw_stop("a " + std::to_string(width) + "-byte store to " + describe(aligned) +
                           ", which takes only whole words here",
                       pc, physical);
    }
    std::uint32_t word = value;
    if (width != 4) {
        const auto old = register_read(aligned, pc);
        if (!old) {
            return false;
        }
        word = merge(*old, physical, width, value);
    }
    if (!register_write(aligned, word, pc)) {
        return false;
    }
    owner_.hw_activity();
    return true;
}

std::optional<std::uint32_t> IopHardware::register_read(std::uint32_t physical, std::uint32_t pc) {
    (void)pc;
    if (physical >= iop_map::ssbus_a && physical < iop_map::ssbus_a + 4 * iop_map::ssbus_words) {
        return ssbus_a_[(physical - iop_map::ssbus_a) / 4];
    }
    if (physical >= iop_map::ssbus_b && physical < iop_map::ssbus_b + 4 * iop_map::ssbus_words) {
        return ssbus_b_[(physical - iop_map::ssbus_b) / 4];
    }
    std::uint32_t channel = 0;
    if (const auto field = dma_register(physical, channel)) {
        const Channel& c = channels_[channel];
        switch (*field) {
        case dma_madr:
            return c.madr;
        case dma_bcr:
            return c.bcr;
        case dma_chcr:
            return c.chcr;
        default:
            return c.tadr;
        }
    }
    switch (physical) {
    case iop_map::i_stat:
        return i_stat_;
    case iop_map::i_mask:
        return i_mask_;
    case iop_map::i_ctrl: {
        // A read returns the enable and clears it (the kernel's atomic interrupt disable).
        const std::uint32_t v = i_ctrl_;
        i_ctrl_ = 0;
        return v;
    }
    case iop_map::dpcr:
        return dpcr_;
    case iop_map::dicr:
        return dicr_read();
    case iop_map::dpcr2:
        return dpcr2_;
    case iop_map::dicr2:
        return dicr2_;
    case iop_map::dmacen:
        return dmacen_;
    case iop_map::dmacinten:
        return dmacinten_;
    default:
        return std::nullopt;
    }
}

bool IopHardware::register_write(std::uint32_t physical, std::uint32_t value, std::uint32_t pc) {
    if (physical >= iop_map::ssbus_a && physical < iop_map::ssbus_a + 4 * iop_map::ssbus_words) {
        ssbus_a_[(physical - iop_map::ssbus_a) / 4] = value;
        return true;
    }
    if (physical >= iop_map::ssbus_b && physical < iop_map::ssbus_b + 4 * iop_map::ssbus_words) {
        ssbus_b_[(physical - iop_map::ssbus_b) / 4] = value;
        return true;
    }
    std::uint32_t channel = 0;
    if (const auto field = dma_register(physical, channel)) {
        Channel& c = channels_[channel];
        switch (*field) {
        case dma_madr:
            c.madr = value;
            break;
        case dma_bcr:
            c.bcr = value;
            break;
        case dma_chcr:
            write_chcr(channel, value, pc);
            break;
        default:
            c.tadr = value;
            break;
        }
        return true;
    }
    switch (physical) {
    case iop_map::i_stat:
        i_stat_ &= value; // writing 0 to a bit acknowledges it
        return true;
    case iop_map::i_mask:
        i_mask_ = value;
        if (pending_lines() != 0) {
            owner_.hw_interrupt(owner_.hw_now());
        }
        return true;
    case iop_map::i_ctrl:
        i_ctrl_ = value & 1u;
        if (pending_lines() != 0) {
            owner_.hw_interrupt(owner_.hw_now());
        }
        return true;
    case iop_map::dpcr:
        dpcr_ = value;
        return true;
    case iop_map::dicr:
        // Bits 24-30 are flags a 1 clears; bit 31 is computed.
        dicr_ = (value & 0x00FFFFFFu) | (dicr_ & ~value & 0x7F000000u);
        return true;
    case iop_map::dpcr2:
        dpcr2_ = value;
        return true;
    case iop_map::dicr2:
        dicr2_ = (value & 0x00FFFFFFu) | (dicr2_ & ~value & 0x3F000000u);
        return true;
    case iop_map::dmacen:
        dmacen_ = value;
        return true;
    case iop_map::dmacinten:
        dmacinten_ = value;
        return true;
    default:
        return false;
    }
}

bool IopHardware::dpcr_enabled(std::uint32_t channel) const noexcept {
    return channel < 7 ? ((dpcr_ >> (4 * channel + 3)) & 1u) != 0
                       : ((dpcr2_ >> (4 * (channel - 7) + 3)) & 1u) != 0;
}

std::uint32_t IopHardware::dicr_read() const noexcept {
    const bool flagged = dma_flags() != 0 && (dicr_ & dicr_master_enable) != 0;
    return (dicr_ & 0x7FFFFFFFu) | (flagged || (dicr_ & 0x8000u) != 0 ? 0x80000000u : 0u);
}

void IopHardware::write_chcr(std::uint32_t channel, std::uint32_t value, std::uint32_t pc) {
    Channel& c = channels_[channel];
    const bool was_busy = (c.chcr & chcr_busy) != 0;
    const bool busy = (value & chcr_busy) != 0;
    c.chcr = value;
    if (!was_busy && busy) {
        start_dma(channel, pc);
    } else if (was_busy && !busy) {
        if (c.event != 0) {
            machine_.events().cancel(c.event);
            c.event = 0;
        }
        if (dma_device_[channel] >= 0) {
            devices_[static_cast<std::size_t>(dma_device_[channel])].dma->dma_stop(channel, owner_.hw_now());
        }
    }
}

void IopHardware::start_dma(std::uint32_t channel, std::uint32_t pc) {
    Channel& c = channels_[channel];
    if (dma_device_[channel] < 0) {
        owner_.hw_stop("IOP DMA channel " + std::to_string(channel) + " was started (CHCR " + hex32(c.chcr) +
                           ", MADR " + hex32(c.madr) + ", BCR " + hex32(c.bcr) + "); no device serves it",
                       pc, iop_map::dma_low);
    }
    if (!dpcr_enabled(channel)) {
        owner_.hw_stop("IOP DMA channel " + std::to_string(channel) + " was started while DPCR" +
                           (channel < 7 ? "" : "2") + " leaves it disabled",
                       pc, iop_map::dpcr);
    }
    const Cycles now = owner_.hw_now();
    IIopDmaDevice& dev = *devices_[static_cast<std::size_t>(dma_device_[channel])].dma;
    const IopDmaStart start = dev.dma_start(IopDmaRequest{channel, c.madr, c.bcr, c.chcr, now});
    if (start.deferred) {
        return;
    }
    c.event =
        machine_.events().schedule(std::max(start.done_at, now), "iop_dma", [this, channel](Cycles when) {
            channels_[channel].event = 0;
            dma_complete(channel, when);
        });
}

void IopHardware::dma_complete(std::uint32_t channel, Cycles at) {
    if (channel >= iop_dma_channels) {
        throw std::logic_error("IOP DMA completion on channel " + std::to_string(channel));
    }
    Channel& c = channels_[channel];
    if ((c.chcr & chcr_busy) == 0) {
        return; // the guest stopped it first
    }
    c.chcr &= ~chcr_busy;
    if (!dma_interrupt_enabled(channel)) {
        owner_.hw_activity();
        return;
    }
    if (channel < 7) {
        dicr_ |= 1u << (24 + channel);
    } else {
        dicr2_ |= 1u << (24 + channel - 7);
    }
    if ((dicr_ & dicr_master_enable) != 0) {
        raise_irq(IopIrq::dma, at);
    } else {
        owner_.hw_activity();
    }
}

void IopHardware::raise_irq(IopIrq line, Cycles at) {
    i_stat_ |= 1u << static_cast<std::uint32_t>(line);
    owner_.hw_interrupt(at);
}

std::uint32_t IopHardware::dma_flags() const noexcept {
    return ((dicr_ >> 24) & 0x7Fu) | (((dicr2_ >> 24) & 0x3Fu) << 7);
}

void IopHardware::acknowledge_dma(std::uint32_t channel) noexcept {
    if (channel < 7) {
        dicr_ &= ~(1u << (24 + channel));
    } else if (channel < 13) {
        dicr2_ &= ~(1u << (24 + channel - 7));
    }
}

bool IopHardware::dma_interrupt_enabled(std::uint32_t channel) const noexcept {
    return channel < 7 ? ((dicr_ >> (16 + channel)) & 1u) != 0 : ((dicr2_ >> (16 + channel - 7)) & 1u) != 0;
}

void IopHardware::set_dma_interrupt(std::uint32_t channel, bool enabled) noexcept {
    if (channel < 7) {
        const std::uint32_t bit = 1u << (16 + channel);
        dicr_ = enabled ? (dicr_ | bit | dicr_master_enable) : (dicr_ & ~bit);
    } else if (channel < 13) {
        const std::uint32_t bit = 1u << (16 + channel - 7);
        dicr2_ = enabled ? (dicr2_ | bit) : (dicr2_ & ~bit);
        if (enabled) {
            dicr_ |= dicr_master_enable;
        }
    }
}

std::string IopHardware::describe(std::uint32_t physical) const {
    if (const IopDeviceMapping* d = device_at(physical)) {
        return std::string(d->device->name()) + "+" + hex32(physical - d->base);
    }
    std::uint32_t channel = 0;
    if (const auto field = dma_register(physical & ~3u, channel)) {
        static constexpr const char* names[4] = {"MADR", "BCR", "CHCR", "TADR"};
        return channel_name(channel) + " " + names[*field / 4];
    }
    if (physical >= iop_map::ssbus_a && physical < iop_map::ssbus_a + 4 * iop_map::ssbus_words) {
        return "SSBUS " + hex32(physical);
    }
    if (physical >= iop_map::ssbus_b && physical < iop_map::ssbus_b + 4 * iop_map::ssbus_words) {
        return "SSBUS " + hex32(physical);
    }
    switch (physical & ~3u) {
    case iop_map::i_stat:
        return "I_STAT";
    case iop_map::i_mask:
        return "I_MASK";
    case iop_map::i_ctrl:
        return "I_CTRL";
    case iop_map::dpcr:
        return "DPCR";
    case iop_map::dicr:
        return "DICR";
    case iop_map::dpcr2:
        return "DPCR2";
    case iop_map::dicr2:
        return "DICR2";
    case iop_map::dmacen:
        return "DMACEN";
    case iop_map::dmacinten:
        return "DMACINTEN";
    default:
        break;
    }
    if (physical >= iop_map::hle_page && physical - iop_map::hle_page < iop_map::hle_page_size) {
        return "the HLE page";
    }
    return "unmapped IOP address " + hex32(physical);
}

} // namespace dq8::rt
