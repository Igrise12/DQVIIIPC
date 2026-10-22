// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

// IOP RAM (2 MiB, visible to the EE at physical 0x1C000000). With native IOP services nothing executes in it;
// it holds what the EE sends over SIF (command packets, RPC argument buffers) and what services allocate for
// themselves. The heap is first-fit and deterministic. Implemented by M3 session C.
namespace dq8::rt {

class IopMemory {
  public:
    static constexpr std::uint32_t size = 2u << 20;
    static constexpr std::uint32_t heap_base = 0x00010000u;
    static constexpr std::uint32_t heap_end = 0x001F0000u;

    IopMemory();
    std::uint8_t* data() noexcept { return ram_.get(); }
    const std::uint8_t* data() const noexcept { return ram_.get(); }

    // Byte copies; the range must lie inside IOP RAM (std::out_of_range otherwise).
    void write(std::uint32_t addr, std::span<const std::uint8_t> bytes);
    void read(std::uint32_t addr, std::span<std::uint8_t> out) const;
    std::uint32_t read32(std::uint32_t addr) const;
    void write32(std::uint32_t addr, std::uint32_t value);

    // 16-byte aligned blocks from [heap_base, heap_end); nullopt when full.
    std::optional<std::uint32_t> allocate(std::uint32_t bytes);
    // M4 (the interpreted IOP's sysmem): blocks of `bytes` rounded up to `align` (a power of two), first fit
    // from the lowest address or, with `high`, from the highest; or exactly at `addr`.
    std::optional<std::uint32_t> allocate(std::uint32_t bytes, std::uint32_t align, bool high);
    std::optional<std::uint32_t> allocate_at(std::uint32_t addr, std::uint32_t bytes, std::uint32_t align);
    // Frees the block that starts at `addr`; false when none does.
    bool free(std::uint32_t addr);
    // The size of the block that starts at `addr`.
    std::optional<std::uint32_t> block_size(std::uint32_t addr) const;
    void reset(); // zero RAM, empty heap

  private:
    std::unique_ptr<std::uint8_t[]> ram_;
    struct Heap;
    std::shared_ptr<Heap> heap_;
};

} // namespace dq8::rt
