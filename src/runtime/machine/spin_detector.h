// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>

// Detects a guest polling loop on a volatile register (GS CSR, INTC or DMAC status, a channel's CHCR, a timer
// count, a SIF register): the same pc reading the same address and getting the same value N times in a row
// with nothing happening in between. The machine then jumps the clock to the next event instead of burning
// host time on iterations that cannot observe a change. The decision depends only on guest behaviour, so it
// is deterministic. RAM polls never reach the host and are bounded by the back-edge cost instead.
namespace dq8::rt {

class SpinDetector {
  public:
    explicit SpinDetector(std::uint32_t threshold = 64) noexcept : threshold_(threshold) {}

    // Records one read. Returns true on the read that completes `threshold` identical reads in a row; the
    // count then starts over.
    bool note(std::uint32_t pc, std::uint32_t address, std::uint64_t value) noexcept {
        if (count_ != 0 && pc == pc_ && address == address_ && value == value_) {
            if (++count_ >= threshold_) {
                count_ = 0;
                return true;
            }
            return false;
        }
        pc_ = pc;
        address_ = address;
        value_ = value;
        count_ = 1;
        return false;
    }

    // Something happened (an event ran, a thread switched, the guest wrote a register): start over.
    void reset() noexcept { count_ = 0; }

    std::uint32_t threshold() const noexcept { return threshold_; }

  private:
    std::uint32_t threshold_;
    std::uint32_t pc_ = 0;
    std::uint32_t address_ = 0;
    std::uint64_t value_ = 0;
    std::uint32_t count_ = 0;
};

} // namespace dq8::rt
