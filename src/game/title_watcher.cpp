// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "game/title_watcher.h"

#include "runtime/ee/guest_memory.h"

#include <array>
#include <stdexcept>

namespace dq8::game {

namespace {

std::uint32_t read_le(rt::GuestMemory& memory, std::uint32_t address, std::uint32_t width) {
    std::array<std::uint8_t, 4> b{};
    memory.read_bytes(address, std::span<std::uint8_t>(b.data(), width));
    return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
}

std::uint32_t overlay_id_of(const core::RegionProfile& profile) {
    if (profile.title_screen.variable == 0) {
        throw std::invalid_argument("the region profile " + profile.id + " has no [title] marker");
    }
    for (const core::ProfileOverlay& o : profile.overlays) {
        if (o.name == profile.title_screen.overlay) {
            return o.id;
        }
    }
    throw std::invalid_argument("[title].overlay " + profile.title_screen.overlay + " is not an overlay of " +
                                profile.id);
}

} // namespace

TitleWatcher::TitleWatcher(const core::RegionProfile& profile)
    : id_address_(profile.overlay_window.base + 4), overlay_id_(overlay_id_of(profile)),
      variable_(profile.title_screen.variable), width_(profile.title_screen.width),
      value_(profile.title_screen.value) {}

void TitleWatcher::attach(rt::MachineServices& machine) {
    machine_ = &machine;
    machine.add_vblank_listener(rt::VblankPhase::intc, *this);
}

void TitleWatcher::reset() {
    seen_ = false;
}

void TitleWatcher::on_vblank_start(std::uint64_t field) {
    if (seen_) {
        return;
    }
    rt::GuestMemory& memory = machine_->memory();
    if (read_le(memory, id_address_, 4) != overlay_id_ || read_le(memory, variable_, width_) != value_) {
        return;
    }
    seen_ = true;
    machine_->emit(rt::MachineEvent{"title", {}}.add("field", field));
}

} // namespace dq8::game
