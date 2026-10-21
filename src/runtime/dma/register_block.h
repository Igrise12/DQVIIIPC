// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/machine/mmio.h"

#include <cstdint>
#include <cstring>
#include <string_view>

// EE register blocks (timers, INTC, DMAC, VIF, GIF) are 32-bit registers on 16-byte strides. A block
// implements word accesses; RegisterWindow turns the guest's 8- to 128-bit accesses into them and maps one
// physical range of the block onto the MMIO bus (a block may own several ranges, such as the DMAC's channel
// registers and D_ENABLER).
namespace dq8::rt {

class IRegisterBlock {
  public:
    virtual ~IRegisterBlock() = default;
    virtual std::string_view name() const = 0;
    // `offset` is 4-byte aligned and relative to the block base. False: no register there (unhandled).
    virtual bool read32(std::uint32_t offset, std::uint32_t& value, const MmioContext& at) = 0;
    // `mask` has the written bytes set (0xFFFFFFFF for a word store; 0x0000FF00 for a byte store to byte
    // 1). `value` is already shifted into place and zero outside the mask.
    virtual bool write32(std::uint32_t offset, std::uint32_t value, std::uint32_t mask,
                         const MmioContext& at) = 0;
    // Whole-quadword FIFO accesses (VIF and GIF FIFOs). Default: not a FIFO.
    virtual bool read128(std::uint32_t /*offset*/, std::uint8_t* /*out*/, const MmioContext& /*at*/) {
        return false;
    }
    virtual bool write128(std::uint32_t /*offset*/, const std::uint8_t* /*in*/, const MmioContext& /*at*/) {
        return false;
    }
};

// Maps [base, base + size) of the physical space to `block`, offset by `block_offset` (the distance from
// the block's base to `base`).
class RegisterWindow final : public IMmioDevice {
  public:
    RegisterWindow(IRegisterBlock& block, std::uint32_t block_offset) noexcept
        : block_(block), block_offset_(block_offset) {}

    std::string_view name() const override { return block_.name(); }

    bool read(std::uint32_t offset, std::uint32_t width, std::uint8_t* out, const MmioContext& at) override {
        const std::uint32_t off = block_offset_ + offset;
        if (width == 16 && (off & 15u) == 0 && block_.read128(off, out, at)) {
            return true;
        }
        if (width < 4) {
            std::uint32_t word = 0;
            if (!block_.read32(off & ~3u, word, at)) {
                return false;
            }
            word >>= (off & 3u) * 8u;
            std::memcpy(out, &word, width);
            return true;
        }
        // 4, 8 or 16 bytes: the first word must be a register; the rest of the slot reads as zero.
        for (std::uint32_t i = 0; i < width; i += 4) {
            std::uint32_t word = 0;
            if (!block_.read32(off + i, word, at)) {
                if (i == 0) {
                    return false;
                }
                word = 0;
            }
            std::memcpy(out + i, &word, 4);
        }
        return true;
    }

    bool write(std::uint32_t offset, std::uint32_t width, const std::uint8_t* in,
               const MmioContext& at) override {
        const std::uint32_t off = block_offset_ + offset;
        if (width == 16 && (off & 15u) == 0 && block_.write128(off, in, at)) {
            return true;
        }
        if (width < 4) {
            std::uint32_t value = 0;
            std::memcpy(&value, in, width);
            const std::uint32_t shift = (off & 3u) * 8u;
            const std::uint32_t mask = (width == 1 ? 0xFFu : 0xFFFFu) << shift;
            return block_.write32(off & ~3u, value << shift, mask, at);
        }
        // Wider stores write the registers they cover; words with no register in them are ignored as long
        // as the first one is a register (a 64- or 128-bit store to a 32-bit register).
        for (std::uint32_t i = 0; i < width; i += 4) {
            std::uint32_t value = 0;
            std::memcpy(&value, in + i, 4);
            if (!block_.write32(off + i, value, 0xFFFFFFFFu, at) && i == 0) {
                return false;
            }
        }
        return true;
    }

  private:
    IRegisterBlock& block_;
    std::uint32_t block_offset_;
};

// A plain register update under a byte mask.
constexpr std::uint32_t merge_masked(std::uint32_t old_value, std::uint32_t value,
                                     std::uint32_t mask) noexcept {
    return (old_value & ~mask) | (value & mask);
}

} // namespace dq8::rt
