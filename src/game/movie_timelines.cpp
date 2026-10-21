// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "game/movie_timelines.h"

#include "runtime/ee/guest_memory.h"

#include <array>
#include <span>
#include <stdexcept>

namespace dq8::game {

namespace {

std::uint32_t read_le(const rt::GuestMemory& memory, std::uint32_t address, std::uint32_t width) {
    std::array<std::uint8_t, 4> b{};
    memory.read_bytes(address, std::span<std::uint8_t>(b.data(), width));
    return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
}

void write_le32(rt::GuestMemory& memory, std::uint32_t address, std::uint32_t value) {
    const std::array<std::uint8_t, 4> b = {
        static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
        static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)};
    memory.write_bytes(address, b);
}

std::uint32_t overlay_id_of(const core::RegionProfile& profile, const std::string& name) {
    for (const core::ProfileOverlay& o : profile.overlays) {
        if (o.name == name) {
            return o.id;
        }
    }
    throw std::invalid_argument("[[movie.timelines]].overlay " + name + " is not an overlay of " +
                                profile.id);
}

} // namespace

MovieTimelineSkip::MovieTimelineSkip(const core::RegionProfile& profile)
    : id_address_(profile.overlay_window.base + 4) {
    if (profile.movie.timelines.empty()) {
        throw std::invalid_argument("the region profile " + profile.id + " has no [[movie.timelines]]");
    }
    for (const core::RegionProfile::MovieTimeline& t : profile.movie.timelines) {
        timelines_.push_back(Timeline{t.name, overlay_id_of(profile, t.overlay), t.stage_variable,
                                      t.stage_width, t.stage_value, t.counter, t.skip_to});
    }
}

void MovieTimelineSkip::attach(rt::MachineServices& machine) {
    machine_ = &machine;
    machine.add_vblank_listener(rt::VblankPhase::intc, *this);
}

void MovieTimelineSkip::reset() {
    skips_ = 0;
}

void MovieTimelineSkip::on_vblank_start(std::uint64_t /*field*/) {
    rt::GuestMemory& memory = machine_->memory();
    const std::uint32_t resident = read_le(memory, id_address_, 4);
    for (const Timeline& t : timelines_) {
        if (resident != t.overlay_id || read_le(memory, t.stage_variable, t.stage_width) != t.stage_value) {
            continue;
        }
        const auto step = static_cast<std::int32_t>(read_le(memory, t.counter, 4));
        if (step <= 0 || step >= t.skip_to) {
            continue;
        }
        write_le32(memory, t.counter, static_cast<std::uint32_t>(t.skip_to));
        ++skips_;
        machine_->emit(rt::MachineEvent{"movie_timeline_skip", {}}
                           .add("timeline", t.name)
                           .add("from", std::int64_t{step})
                           .add("to", std::int64_t{t.skip_to}));
    }
}

} // namespace dq8::game
