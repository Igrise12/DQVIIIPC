// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/iop_memory.h"

#include "runtime/ee/guest_stop.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

namespace dq8::rt {

namespace {

void check_range(std::uint32_t addr, std::size_t bytes) {
    if (addr > IopMemory::size || bytes > IopMemory::size - addr) {
        throw std::out_of_range("IOP RAM range " + hex32(addr) + " + " + std::to_string(bytes) +
                                " lies outside the 2 MiB of IOP RAM");
    }
}

} // namespace

// First fit over the gaps between allocated blocks, which are kept sorted by address. A free merges nothing
// because the free space is never stored: it is the gaps themselves.
struct IopMemory::Heap {
    std::map<std::uint32_t, std::uint32_t> blocks; // address -> size, both 16-byte multiples
};

IopMemory::IopMemory() : ram_(new std::uint8_t[size]()), heap_(std::make_shared<Heap>()) {}

void IopMemory::write(std::uint32_t addr, std::span<const std::uint8_t> bytes) {
    check_range(addr, bytes.size());
    if (!bytes.empty()) {
        std::memcpy(ram_.get() + addr, bytes.data(), bytes.size());
    }
}

void IopMemory::read(std::uint32_t addr, std::span<std::uint8_t> out) const {
    check_range(addr, out.size());
    if (!out.empty()) {
        std::memcpy(out.data(), ram_.get() + addr, out.size());
    }
}

std::uint32_t IopMemory::read32(std::uint32_t addr) const {
    std::uint8_t b[4];
    read(addr, b);
    return static_cast<std::uint32_t>(b[0]) | static_cast<std::uint32_t>(b[1]) << 8 |
           static_cast<std::uint32_t>(b[2]) << 16 | static_cast<std::uint32_t>(b[3]) << 24;
}

void IopMemory::write32(std::uint32_t addr, std::uint32_t value) {
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
                               static_cast<std::uint8_t>(value >> 16),
                               static_cast<std::uint8_t>(value >> 24)};
    write(addr, b);
}

std::optional<std::uint32_t> IopMemory::allocate(std::uint32_t bytes) {
    if (bytes == 0 || bytes > heap_end - heap_base) {
        return std::nullopt;
    }
    const std::uint32_t rounded = (bytes + 15u) & ~15u;
    std::uint32_t cursor = heap_base;
    for (const auto& [addr, block_size] : heap_->blocks) {
        if (addr - cursor >= rounded) {
            break;
        }
        cursor = addr + block_size;
    }
    if (heap_end - cursor < rounded) {
        return std::nullopt;
    }
    heap_->blocks.emplace(cursor, rounded);
    return cursor;
}

std::optional<std::uint32_t> IopMemory::allocate(std::uint32_t bytes, std::uint32_t align, bool high) {
    if (bytes == 0 || align == 0 || (align & (align - 1)) != 0 || bytes > heap_end - heap_base) {
        return std::nullopt;
    }
    const std::uint32_t rounded = (bytes + align - 1) & ~(align - 1);
    if (!high) {
        std::uint32_t cursor = heap_base;
        for (const auto& [addr, block_size] : heap_->blocks) {
            const std::uint32_t start = (cursor + align - 1) & ~(align - 1);
            if (start <= addr && addr - start >= rounded) {
                heap_->blocks.emplace(start, rounded);
                return start;
            }
            cursor = std::max(cursor, addr + block_size);
        }
        const std::uint32_t start = (cursor + align - 1) & ~(align - 1);
        if (start > heap_end || heap_end - start < rounded) {
            return std::nullopt;
        }
        heap_->blocks.emplace(start, rounded);
        return start;
    }
    std::uint32_t end = heap_end;
    for (auto it = heap_->blocks.rbegin(); it != heap_->blocks.rend(); ++it) {
        const std::uint32_t block_end = it->first + it->second;
        if (end >= rounded) {
            const std::uint32_t start = (end - rounded) & ~(align - 1);
            if (start >= block_end) {
                heap_->blocks.emplace(start, rounded);
                return start;
            }
        }
        end = std::min(end, it->first);
    }
    if (end < rounded) {
        return std::nullopt;
    }
    const std::uint32_t start = (end - rounded) & ~(align - 1);
    if (start < heap_base) {
        return std::nullopt;
    }
    heap_->blocks.emplace(start, rounded);
    return start;
}

std::optional<std::uint32_t> IopMemory::allocate_at(std::uint32_t addr, std::uint32_t bytes,
                                                    std::uint32_t align) {
    if (bytes == 0 || align == 0 || (align & (align - 1)) != 0) {
        return std::nullopt;
    }
    const std::uint32_t start = addr & ~(align - 1);
    const std::uint64_t end = std::uint64_t{addr} + bytes;
    const std::uint32_t rounded =
        static_cast<std::uint32_t>(((end - start) + align - 1) & ~std::uint64_t{align - 1});
    if (start < heap_base || start > heap_end || heap_end - start < rounded) {
        return std::nullopt;
    }
    for (const auto& [a, block_size] : heap_->blocks) {
        if (a < start + rounded && start < a + block_size) {
            return std::nullopt;
        }
    }
    heap_->blocks.emplace(start, rounded);
    return start;
}

bool IopMemory::free(std::uint32_t addr) {
    return heap_->blocks.erase(addr) != 0;
}

std::optional<std::uint32_t> IopMemory::block_size(std::uint32_t addr) const {
    const auto it = heap_->blocks.find(addr);
    if (it == heap_->blocks.end()) {
        return std::nullopt;
    }
    return it->second;
}

void IopMemory::reset() {
    std::memset(ram_.get(), 0, size);
    heap_->blocks.clear();
}

} // namespace dq8::rt
