// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/kernel/fiber_stack.h"

#include "runtime/kernel/fiber.h"

#include <stdexcept>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <intrin.h>
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace dq8::rt {

std::size_t Fiber::headroom() const noexcept {
    const std::uintptr_t frame = current_frame_address();
    return frame > stack_low() ? static_cast<std::size_t>(frame - stack_low()) : 0;
}

std::uintptr_t current_frame_address() noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
    return reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
#else
    return reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
#endif
}

namespace detail {

std::size_t page_size() noexcept {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return static_cast<std::size_t>(info.dwPageSize);
#else
    const long page = sysconf(_SC_PAGESIZE);
    return page > 0 ? static_cast<std::size_t>(page) : std::size_t{4096};
#endif
}

GuardedRegion::GuardedRegion(std::size_t size, bool guard, std::size_t guard_offset) {
    const std::size_t page = page_size();
    size = (size + page - 1) / page * page;
    if (size == 0 || (guard && (guard_offset % page != 0 || guard_offset + page > size))) {
        throw std::invalid_argument("fiber stack: bad region layout");
    }
#if defined(_WIN32)
    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p == nullptr) {
        throw std::runtime_error("fiber stack: VirtualAlloc of " + std::to_string(size) + " bytes failed");
    }
    base_ = static_cast<std::uint8_t*>(p);
    size_ = size;
    if (guard) {
        DWORD old = 0;
        if (VirtualProtect(base_ + guard_offset, page, PAGE_NOACCESS, &old) == 0) {
            release();
            throw std::runtime_error("fiber stack: VirtualProtect of the guard page failed");
        }
    }
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        throw std::runtime_error("fiber stack: mmap of " + std::to_string(size) + " bytes failed");
    }
    base_ = static_cast<std::uint8_t*>(p);
    size_ = size;
    if (guard && mprotect(base_ + guard_offset, page, PROT_NONE) != 0) {
        release();
        throw std::runtime_error("fiber stack: mprotect of the guard page failed");
    }
#endif
}

GuardedRegion::~GuardedRegion() {
    release();
}

GuardedRegion::GuardedRegion(GuardedRegion&& other) noexcept
    : base_(std::exchange(other.base_, nullptr)), size_(std::exchange(other.size_, 0)) {}

GuardedRegion& GuardedRegion::operator=(GuardedRegion&& other) noexcept {
    if (this != &other) {
        release();
        base_ = std::exchange(other.base_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

void GuardedRegion::release() noexcept {
    if (base_ == nullptr) {
        return;
    }
#if defined(_WIN32)
    VirtualFree(base_, 0, MEM_RELEASE);
#else
    munmap(base_, size_);
#endif
    base_ = nullptr;
    size_ = 0;
}

} // namespace detail
} // namespace dq8::rt
