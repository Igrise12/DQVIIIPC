// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/abi/ps2recomp/r5900_context.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// Device registers (architecture §4.1): generated code calls the host for every address
// ps2_is_special_address() accepts, and GuestMemory hands the ones outside RAM and the scratchpad to this bus
// by physical address. A device claims a range; an access no device claims, or one a device declines, stays
// an "unhandled" access that GuestMemory logs (and traps with --trap-mmio), exactly as in M2.
namespace dq8::rt {

// Who is accessing: the guest pc and its register context (null for host-side accesses).
struct MmioContext {
    std::uint32_t pc = 0;
    R5900Context* ctx = nullptr;
};

class IMmioDevice {
  public:
    virtual ~IMmioDevice() = default;
    virtual std::string_view name() const = 0;
    // `offset` is relative to the start of the mapped range; `width` is 1, 2, 4, 8 or 16 bytes and the access
    // never crosses the range's end. Return false for a register the device does not model: the access is
    // then logged as unhandled (a read returns zeros).
    virtual bool read(std::uint32_t offset, std::uint32_t width, std::uint8_t* out,
                      const MmioContext& at) = 0;
    virtual bool write(std::uint32_t offset, std::uint32_t width, const std::uint8_t* in,
                       const MmioContext& at) = 0;
};

// Physical device space [0x10000000, 0x20000000): EE registers, VU memory, GS privileged registers, IOP RAM
// and the boot ROM range. Lookup goes through a 4 KiB page table, then a short list of the ranges on that
// page.
class MmioBus {
  public:
    static constexpr std::uint32_t space_base = 0x10000000u;
    static constexpr std::uint32_t space_end = 0x20000000u;
    static constexpr std::uint32_t page_size = 0x1000u;

    MmioBus();

    // Maps [phys_base, phys_base + size) to `device` (which must outlive the bus). Ranges are 16-byte
    // aligned, lie inside the device space and never overlap; anything else throws std::invalid_argument.
    void map(std::uint32_t phys_base, std::uint32_t size, IMmioDevice& device);

    struct Hit {
        IMmioDevice* device = nullptr;
        std::uint32_t offset = 0;    // from the start of the range
        std::uint32_t range_end = 0; // one past the range's last address
    };
    std::optional<Hit> find(std::uint32_t phys) const noexcept;

    // Routes one access. False when no device claims the address, the access would cross the end of the
    // range, or the device declined it.
    bool read(std::uint32_t phys, std::uint32_t width, std::uint8_t* out, const MmioContext& at) const;
    bool write(std::uint32_t phys, std::uint32_t width, const std::uint8_t* in, const MmioContext& at) const;

    struct Mapping {
        std::uint32_t base = 0;
        std::uint32_t size = 0;
        IMmioDevice* device = nullptr;
    };
    // Every mapping, sorted by base (for diagnostics).
    std::vector<Mapping> mappings() const;

  private:
    std::vector<Mapping> ranges_;                   // sorted by base
    std::vector<std::vector<std::uint32_t>> pages_; // per page: indices into ranges_
};

} // namespace dq8::rt
