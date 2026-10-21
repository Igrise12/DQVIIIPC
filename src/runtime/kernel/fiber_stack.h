// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstddef>
#include <cstdint>

// Fiber stack memory for the minicoro and ucontext backends: one mapping of whole pages, read-write, with an
// optional inaccessible guard page at a chosen offset. Private to src/runtime/kernel/.
namespace dq8::rt::detail {

std::size_t page_size() noexcept;

class GuardedRegion {
  public:
    GuardedRegion() = default;
    // Maps `size` bytes (rounded up to whole pages). With `guard`, the page at byte offset `guard_offset`
    // (page-aligned) becomes inaccessible. Throws std::runtime_error when the mapping fails.
    GuardedRegion(std::size_t size, bool guard, std::size_t guard_offset);
    ~GuardedRegion();
    GuardedRegion(GuardedRegion&& other) noexcept;
    GuardedRegion& operator=(GuardedRegion&& other) noexcept;
    GuardedRegion(const GuardedRegion&) = delete;
    GuardedRegion& operator=(const GuardedRegion&) = delete;

    std::uint8_t* data() const noexcept { return base_; }
    std::size_t size() const noexcept { return size_; }
    void release() noexcept;

  private:
    std::uint8_t* base_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace dq8::rt::detail
