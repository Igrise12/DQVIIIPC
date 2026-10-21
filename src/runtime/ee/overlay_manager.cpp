// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/overlay_manager.h"

#include "runtime/ee/guest_stop.h"

#include <stdexcept>
#include <utility>

namespace dq8::rt {

namespace {

std::string unit_label(const OverlayUnit& u) {
    return u.name + " (id " + std::to_string(u.overlay_id) + ")";
}

} // namespace

OverlayManager::OverlayManager(OverlayWindowSpec window) {
    if ((window.base & 3u) != 0 || (window.end & 3u) != 0 || window.base >= window.end) {
        throw std::invalid_argument("overlay window [" + hex32(window.base) + ", " + hex32(window.end) +
                                    ") is empty or not 4-aligned");
    }
    window_ = window;
}

void OverlayManager::register_unit(std::uint32_t overlay_id, std::string name, DispatchTable table) {
    if (!window_) {
        throw std::invalid_argument("overlay unit " + name + ": there is no overlay window");
    }
    if (overlay_id == 0) {
        throw std::invalid_argument("overlay unit " + name + ": overlay id 0 is reserved for the main unit");
    }
    if (const OverlayUnit* other = find(overlay_id)) {
        throw std::invalid_argument("overlay unit " + name + ": id " + std::to_string(overlay_id) +
                                    " is already registered by " + other->name);
    }
    if (table.empty() || table.base() < window_->base || table.end() > window_->end) {
        throw std::invalid_argument("overlay unit " + name + ": table [" + hex32(table.base()) + ", " +
                                    hex32(table.end()) + ") is not inside the window [" +
                                    hex32(window_->base) + ", " + hex32(window_->end) + ")");
    }
    units_.push_back({overlay_id, std::move(name), table.owned_copy()});
}

const OverlayUnit* OverlayManager::find(std::uint32_t overlay_id) const noexcept {
    for (const OverlayUnit& u : units_) {
        if (u.overlay_id == overlay_id) {
            return &u;
        }
    }
    return nullptr;
}

void OverlayManager::set_resident(std::uint32_t overlay_id) {
    for (std::size_t i = 0; i < units_.size(); ++i) {
        if (units_[i].overlay_id == overlay_id) {
            resident_ = i;
            ++generation_;
            return;
        }
    }
    throw std::invalid_argument("no overlay unit with id " + std::to_string(overlay_id) + " is registered");
}

void OverlayManager::clear_resident() noexcept {
    resident_.reset();
    ++generation_;
}

const OverlayUnit* OverlayManager::resident() const noexcept {
    return resident_ ? &units_[*resident_] : nullptr;
}

dq8_guest_fn OverlayManager::lookup(std::uint32_t addr) const noexcept {
    if (!in_window(addr) || !resident_) {
        return nullptr;
    }
    return units_[*resident_].table.lookup(addr);
}

std::string OverlayManager::describe_miss(std::uint32_t addr) const {
    if (!in_window(addr)) {
        return hex32(addr) + " is outside the overlay window";
    }
    std::string out = hex32(addr) + " is in the overlay window [" + hex32(window_->base) + ", " +
                      hex32(window_->end) + "); ";
    const OverlayUnit* res = resident();
    out += res ? "resident overlay " + unit_label(*res) + " has no function there" : "no overlay is resident";
    std::string others;
    for (const OverlayUnit& u : units_) {
        if (&u != res && u.table.lookup(addr) != nullptr) {
            others += (others.empty() ? "" : ", ") + unit_label(u);
        }
    }
    out += others.empty() ? "; no registered overlay has a function there" : "; it is an entry of " + others;
    return out;
}

} // namespace dq8::rt
