// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/machine/clock.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Guest memory (architecture.md §4.1): 32 MiB of RDRAM and the 16 KiB scratchpad. Generated code reads and
// writes RDRAM directly (rdram[addr & PS2_RAM_MASK]) and calls the host only for addresses
// ps2_is_special_address() accepts; those land here. Device addresses go to the MMIO bus when one is set
// (M3): an access a device handles costs `access_cost` cycles of virtual time. Everything else in a device
// range is a logged hole as in M2: loads return 0, stores are dropped, and the first access to each address
// is recorded (or stops the guest with trap-mmio).
namespace dq8::rt {

class MmioBus;

// Where a special address points. Spellings from to_string() appear in dq8-game's output.
enum class MmioRegion {
    io,            // EE hardware registers, DMAC, timers, FIFOs
    vu,            // VU0/VU1 micro and data memory
    gs_privileged, // GS privileged registers
    iop_ram,       // IOP RAM as the EE sees it
    bios,          // boot ROM
    unmapped,      // anything else: gaps in the device map, kseg2/kseg3
};

std::string_view to_string(MmioRegion region) noexcept;

// The region of a special, non-scratchpad address.
MmioRegion classify_mmio(std::uint32_t vaddr) noexcept;

// The first access to one device address, with a running count of every access to it since.
struct MmioAccess {
    std::uint32_t address = 0; // the virtual address the guest used
    MmioRegion region = MmioRegion::unmapped;
    bool store = false;
    std::uint32_t width = 0;    // bytes: 1, 2, 4, 8 or 16
    std::uint64_t value = 0;    // a store's value (its low 64 bits for a 128-bit store)
    std::uint64_t value_hi = 0; // the high 64 bits of a 128-bit store
    std::uint32_t pc = 0;       // ctx->pc at the access
    std::uint64_t count = 0;    // accesses to this address so far
};

class GuestMemory {
  public:
    using MmioSink = std::function<void(const MmioAccess& first_access)>;

    GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    std::uint8_t* rdram() noexcept { return rdram_.get(); }
    const std::uint8_t* rdram() const noexcept { return rdram_.get(); }
    std::uint8_t* scratchpad() noexcept { return scratchpad_.get(); }
    const std::uint8_t* scratchpad() const noexcept { return scratchpad_.get(); }

    // The host side of the generated code's slow path. Any address works: RAM and its mirrors go to RDRAM,
    // the scratchpad to the scratchpad, device addresses to the MMIO bus and, when no device handles them,
    // to the MMIO log. `pc` is passed to the device and recorded with unhandled accesses. With trap-mmio set,
    // an unhandled MMIO access throws GuestStop(mmio_trap) after it is recorded.
    std::uint8_t load8(std::uint32_t vaddr, std::uint32_t pc = 0);
    std::uint16_t load16(std::uint32_t vaddr, std::uint32_t pc = 0);
    std::uint32_t load32(std::uint32_t vaddr, std::uint32_t pc = 0);
    std::uint64_t load64(std::uint32_t vaddr, std::uint32_t pc = 0);
    void load128(std::uint32_t vaddr, std::uint64_t out[2], std::uint32_t pc = 0);
    void store8(std::uint32_t vaddr, std::uint8_t value, std::uint32_t pc = 0);
    void store16(std::uint32_t vaddr, std::uint16_t value, std::uint32_t pc = 0);
    void store32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t pc = 0);
    void store64(std::uint32_t vaddr, std::uint64_t value, std::uint32_t pc = 0);
    void store128(std::uint32_t vaddr, std::uint64_t lo, std::uint64_t hi, std::uint32_t pc = 0);

    // Host-side byte copies for the loader and for tests. The whole range must be RAM (one mirror, no wrap)
    // or scratchpad; anything else throws std::out_of_range. Nothing is logged.
    void write_bytes(std::uint32_t vaddr, std::span<const std::uint8_t> bytes);
    void fill(std::uint32_t vaddr, std::uint32_t size, std::uint8_t value);
    void read_bytes(std::uint32_t vaddr, std::span<std::uint8_t> out) const;

    // Routes device addresses below kseg2 to `bus` by physical address (the machine calls this once; null
    // detaches). Each access a device handles charges `access_cost` to `clock` (when it is not null).
    void set_mmio_bus(const MmioBus* bus, VirtualClock* clock, Cycles access_cost) noexcept {
        bus_ = bus;
        clock_ = clock;
        access_cost_ = access_cost;
    }
    const MmioBus* mmio_bus() const noexcept { return bus_; }
    // Accesses a device handled (they are not in mmio_log()).
    std::uint64_t mmio_handled_count() const noexcept { return mmio_handled_; }

    void set_trap_mmio(bool trap) noexcept { trap_mmio_ = trap; }
    bool trap_mmio() const noexcept { return trap_mmio_; }
    // Called once per address, on its first access. Not called again for repeated accesses.
    void set_mmio_sink(MmioSink sink) { mmio_sink_ = std::move(sink); }
    // Every unhandled device address touched so far, in first-touch order, with counts.
    const std::vector<MmioAccess>& mmio_log() const noexcept { return mmio_log_; }
    std::uint64_t mmio_access_count() const noexcept { return mmio_accesses_; }

  private:
    // One load (bytes is filled) or store (bytes is read) of `width` bytes. RAM and scratchpad wrap within
    // their own size byte by byte, as the generated fast path does for RAM.
    void access(std::uint32_t vaddr, std::uint32_t width, bool store, std::uint8_t* bytes, std::uint32_t pc);
    void record_mmio(std::uint32_t vaddr, std::uint32_t width, bool store, const std::uint8_t* bytes,
                     std::uint32_t pc);
    std::uint8_t* host_range(std::uint32_t vaddr, std::uint32_t size) const;

    std::unique_ptr<std::uint8_t[]> rdram_;
    std::unique_ptr<std::uint8_t[]> scratchpad_;
    const MmioBus* bus_ = nullptr;
    VirtualClock* clock_ = nullptr;
    Cycles access_cost_ = 0;
    std::uint64_t mmio_handled_ = 0;
    bool trap_mmio_ = false;
    MmioSink mmio_sink_;
    std::vector<MmioAccess> mmio_log_;
    std::unordered_map<std::uint32_t, std::size_t> mmio_index_;
    std::uint64_t mmio_accesses_ = 0;
};

} // namespace dq8::rt
