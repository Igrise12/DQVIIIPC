// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/machine/mmio.h"

#include "runtime/ee/guest_stop.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace dq8::rt {

MmioBus::MmioBus() : pages_((space_end - space_base) / page_size) {}

void MmioBus::map(std::uint32_t phys_base, std::uint32_t size, IMmioDevice& device) {
    const std::uint64_t end = static_cast<std::uint64_t>(phys_base) + size;
    if (size == 0 || phys_base % 16 != 0 || size % 16 != 0 || phys_base < space_base || end > space_end) {
        throw std::invalid_argument("mmio: range " + hex32(phys_base) + "+" + hex32(size) + " for " +
                                    std::string(device.name()) +
                                    " must be 16-byte aligned and inside the device space");
    }
    for (const Mapping& m : ranges_) {
        if (phys_base < m.base + m.size && m.base < end) {
            throw std::invalid_argument("mmio: range " + hex32(phys_base) + "+" + hex32(size) + " for " +
                                        std::string(device.name()) + " overlaps " +
                                        std::string(m.device->name()) + " at " + hex32(m.base));
        }
    }
    ranges_.push_back(Mapping{phys_base, size, &device});
    std::sort(ranges_.begin(), ranges_.end(),
              [](const Mapping& a, const Mapping& b) { return a.base < b.base; });
    // Rebuild the page lists: indices into ranges_ changed with the sort.
    for (auto& page : pages_) {
        page.clear();
    }
    for (std::uint32_t i = 0; i < ranges_.size(); ++i) {
        const Mapping& m = ranges_[i];
        const std::uint32_t first = (m.base - space_base) / page_size;
        const std::uint32_t last = (m.base + m.size - 1 - space_base) / page_size;
        for (std::uint32_t p = first; p <= last; ++p) {
            pages_[p].push_back(i);
        }
    }
}

std::optional<MmioBus::Hit> MmioBus::find(std::uint32_t phys) const noexcept {
    if (phys < space_base || phys >= space_end) {
        return std::nullopt;
    }
    for (const std::uint32_t i : pages_[(phys - space_base) / page_size]) {
        const Mapping& m = ranges_[i];
        if (phys >= m.base && phys - m.base < m.size) {
            return Hit{m.device, phys - m.base, m.base + m.size};
        }
    }
    return std::nullopt;
}

bool MmioBus::read(std::uint32_t phys, std::uint32_t width, std::uint8_t* out, const MmioContext& at) const {
    const auto hit = find(phys);
    if (!hit || static_cast<std::uint64_t>(phys) + width > hit->range_end) {
        return false;
    }
    return hit->device->read(hit->offset, width, out, at);
}

bool MmioBus::write(std::uint32_t phys, std::uint32_t width, const std::uint8_t* in,
                    const MmioContext& at) const {
    const auto hit = find(phys);
    if (!hit || static_cast<std::uint64_t>(phys) + width > hit->range_end) {
        return false;
    }
    return hit->device->write(hit->offset, width, in, at);
}

std::vector<MmioBus::Mapping> MmioBus::mappings() const {
    return ranges_;
}

} // namespace dq8::rt
