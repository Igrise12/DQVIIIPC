// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "game/display_watcher.h"

#include "runtime/ee/guest_memory.h"
#include "runtime/ee/guest_stop.h"

#include <array>
#include <span>
#include <stdexcept>

namespace dq8::game {

DisplayWatcher::DisplayWatcher(const core::RegionProfile::Display& display) : display_(display) {
    const std::uint32_t width = display.widescreen_width;
    if (display.widescreen_variable == 0 || (width != 1 && width != 2 && width != 4) ||
        display.widescreen_variable % width != 0) {
        throw std::invalid_argument("the region profile has no usable [display] widescreen variable (" +
                                    rt::hex32(display.widescreen_variable) + ")");
    }
}

void DisplayWatcher::attach(rt::MachineServices& machine) {
    machine_ = &machine;
    machine.add_vblank_listener(rt::VblankPhase::intc, *this);
}

void DisplayWatcher::reset() {
    widescreen_.store(false, std::memory_order_relaxed);
    changes_.store(0, std::memory_order_relaxed);
}

void DisplayWatcher::on_vblank_start(std::uint64_t /*field*/) {
    std::array<std::uint8_t, 4> b{};
    const std::span<std::uint8_t> bytes(b.data(), display_.widescreen_width);
    machine_->memory().read_bytes(display_.widescreen_variable, bytes);
    std::uint32_t value = 0;
    for (std::size_t i = bytes.size(); i-- > 0;) {
        value = (value << 8) | bytes[i];
    }
    const bool wide = value == display_.widescreen_value;
    if (wide != widescreen_.load(std::memory_order_relaxed)) {
        widescreen_.store(wide, std::memory_order_relaxed);
        changes_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace dq8::game
