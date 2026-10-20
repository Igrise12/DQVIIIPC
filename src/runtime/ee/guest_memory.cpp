// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_memory.h"

#include "dq8/abi/ps2recomp/ps2_address.h"
#include "runtime/ee/guest_stop.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace dq8::rt {

std::string_view to_string(MmioRegion region) noexcept {
    switch (region) {
    case MmioRegion::io:
        return "io";
    case MmioRegion::vu:
        return "vu";
    case MmioRegion::gs_privileged:
        return "gs-privileged";
    case MmioRegion::iop_ram:
        return "iop-ram";
    case MmioRegion::bios:
        return "bios";
    case MmioRegion::unmapped:
        return "unmapped";
    }
    return "unmapped";
}

MmioRegion classify_mmio(std::uint32_t vaddr) noexcept {
    if (vaddr >= 0xC0000000u) {
        return MmioRegion::unmapped; // kseg2/kseg3: TLB-mapped, nothing is mapped there in M2
    }
    const std::uint32_t p = ps2_physical_address(vaddr);
    if (p >= PS2_IO_BASE && p < PS2_IO_END) {
        return MmioRegion::io;
    }
    if (p >= PS2_VU_BASE && p < PS2_VU_END) {
        return MmioRegion::vu;
    }
    if (p >= PS2_GS_PRIV_BASE && p < PS2_GS_PRIV_END) {
        return MmioRegion::gs_privileged;
    }
    if (p >= PS2_IOP_RAM_BASE && p < PS2_IOP_RAM_END) {
        return MmioRegion::iop_ram;
    }
    if (p >= PS2_BIOS_BASE && p < PS2_BIOS_END) {
        return MmioRegion::bios;
    }
    return MmioRegion::unmapped;
}

GuestMemory::GuestMemory()
    : rdram_(std::make_unique<std::uint8_t[]>(PS2_RAM_SIZE)),
      scratchpad_(std::make_unique<std::uint8_t[]>(PS2_SCRATCHPAD_SIZE)) {}

void GuestMemory::access(std::uint32_t vaddr, std::uint32_t width, bool store, std::uint8_t* bytes,
                         std::uint32_t pc) {
    std::uint8_t* base = nullptr;
    std::uint32_t mask = 0;
    std::uint32_t offset = 0;
    if (ps2_is_scratchpad_address(vaddr)) {
        base = scratchpad_.get();
        mask = PS2_SCRATCHPAD_SIZE - 1u;
        offset = vaddr - PS2_SCRATCHPAD_BASE;
    } else if (!ps2_is_special_address(vaddr)) {
        base = rdram_.get();
        mask = PS2_RAM_MASK;
        offset = vaddr & PS2_RAM_MASK;
    } else {
        if (!store) {
            std::memset(bytes, 0, width);
        }
        record_mmio(vaddr, width, store, bytes, pc);
        return;
    }
    if (offset + width <= mask + 1u) {
        if (store) {
            std::memcpy(base + offset, bytes, width);
        } else {
            std::memcpy(bytes, base + offset, width);
        }
        return;
    }
    for (std::uint32_t i = 0; i < width; ++i) {
        std::uint8_t& cell = base[(offset + i) & mask];
        if (store) {
            cell = bytes[i];
        } else {
            bytes[i] = cell;
        }
    }
}

void GuestMemory::record_mmio(std::uint32_t vaddr, std::uint32_t width, bool store, const std::uint8_t* bytes,
                              std::uint32_t pc) {
    ++mmio_accesses_;
    const auto [it, inserted] = mmio_index_.try_emplace(vaddr, mmio_log_.size());
    if (!inserted) {
        ++mmio_log_[it->second].count;
    } else {
        MmioAccess a;
        a.address = vaddr;
        a.region = classify_mmio(vaddr);
        a.store = store;
        a.width = width;
        a.pc = pc;
        a.count = 1;
        if (store) {
            std::memcpy(&a.value, bytes, width < 8u ? width : 8u);
            if (width > 8u) {
                std::memcpy(&a.value_hi, bytes + 8, width - 8u);
            }
        }
        mmio_log_.push_back(a);
        if (mmio_sink_) {
            mmio_sink_(mmio_log_.back());
        }
    }
    if (trap_mmio_) {
        StopInfo info;
        info.reason = StopReason::mmio_trap;
        info.pc = pc;
        info.address = vaddr;
        info.message = std::string(store ? "store" : "load") + std::to_string(width * 8u) + " at " +
                       hex32(vaddr) + " (" + std::string(to_string(classify_mmio(vaddr))) +
                       ") with trap-mmio set, pc " + hex32(pc);
        throw GuestStop(std::move(info));
    }
}

std::uint8_t GuestMemory::load8(std::uint32_t vaddr, std::uint32_t pc) {
    std::uint8_t v = 0;
    access(vaddr, 1, false, &v, pc);
    return v;
}

std::uint16_t GuestMemory::load16(std::uint32_t vaddr, std::uint32_t pc) {
    std::uint8_t b[2];
    access(vaddr, 2, false, b, pc);
    std::uint16_t v = 0;
    std::memcpy(&v, b, sizeof v);
    return v;
}

std::uint32_t GuestMemory::load32(std::uint32_t vaddr, std::uint32_t pc) {
    std::uint8_t b[4];
    access(vaddr, 4, false, b, pc);
    std::uint32_t v = 0;
    std::memcpy(&v, b, sizeof v);
    return v;
}

std::uint64_t GuestMemory::load64(std::uint32_t vaddr, std::uint32_t pc) {
    std::uint8_t b[8];
    access(vaddr, 8, false, b, pc);
    std::uint64_t v = 0;
    std::memcpy(&v, b, sizeof v);
    return v;
}

void GuestMemory::load128(std::uint32_t vaddr, std::uint64_t out[2], std::uint32_t pc) {
    std::uint8_t b[16];
    access(vaddr, 16, false, b, pc);
    std::memcpy(out, b, sizeof b);
}

void GuestMemory::store8(std::uint32_t vaddr, std::uint8_t value, std::uint32_t pc) {
    access(vaddr, 1, true, &value, pc);
}

void GuestMemory::store16(std::uint32_t vaddr, std::uint16_t value, std::uint32_t pc) {
    std::uint8_t b[2];
    std::memcpy(b, &value, sizeof b);
    access(vaddr, 2, true, b, pc);
}

void GuestMemory::store32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t pc) {
    std::uint8_t b[4];
    std::memcpy(b, &value, sizeof b);
    access(vaddr, 4, true, b, pc);
}

void GuestMemory::store64(std::uint32_t vaddr, std::uint64_t value, std::uint32_t pc) {
    std::uint8_t b[8];
    std::memcpy(b, &value, sizeof b);
    access(vaddr, 8, true, b, pc);
}

void GuestMemory::store128(std::uint32_t vaddr, std::uint64_t lo, std::uint64_t hi, std::uint32_t pc) {
    std::uint8_t b[16];
    std::memcpy(b, &lo, 8);
    std::memcpy(b + 8, &hi, 8);
    access(vaddr, 16, true, b, pc);
}

std::uint8_t* GuestMemory::host_range(std::uint32_t vaddr, std::uint32_t size) const {
    const std::uint64_t last = std::uint64_t{vaddr} + size - (size != 0 ? 1u : 0u);
    if (last > 0xFFFFFFFFu) {
        throw std::out_of_range("guest range " + hex32(vaddr) + " + " + std::to_string(size) +
                                " wraps the address space");
    }
    const auto last32 = static_cast<std::uint32_t>(last);
    if (ps2_is_scratchpad_address(vaddr) && ps2_is_scratchpad_address(last32)) {
        return scratchpad_.get() + (vaddr - PS2_SCRATCHPAD_BASE);
    }
    if (!ps2_is_special_address(vaddr) && !ps2_is_special_address(last32) &&
        (vaddr & ~PS2_RAM_MASK) == (last32 & ~PS2_RAM_MASK)) {
        return rdram_.get() + (vaddr & PS2_RAM_MASK);
    }
    throw std::out_of_range("guest range " + hex32(vaddr) + " + " + std::to_string(size) +
                            " is not RAM or scratchpad");
}

void GuestMemory::write_bytes(std::uint32_t vaddr, std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) {
        return;
    }
    std::memcpy(host_range(vaddr, static_cast<std::uint32_t>(bytes.size())), bytes.data(), bytes.size());
}

void GuestMemory::fill(std::uint32_t vaddr, std::uint32_t size, std::uint8_t value) {
    if (size == 0) {
        return;
    }
    std::memset(host_range(vaddr, size), value, size);
}

void GuestMemory::read_bytes(std::uint32_t vaddr, std::span<std::uint8_t> out) const {
    if (out.empty()) {
        return;
    }
    std::memcpy(out.data(), host_range(vaddr, static_cast<std::uint32_t>(out.size())), out.size());
}

} // namespace dq8::rt
