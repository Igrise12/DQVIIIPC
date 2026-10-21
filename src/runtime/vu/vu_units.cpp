// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/vu/vu_units.h"

#include "runtime/dma/gif.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/vu/vu0_bridge.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

namespace dq8::rt {

VuUnits::VuUnits(Gif& gif) : gif_(gif), units_{Unit{*this, 0, 0x1000u}, Unit{*this, 1, 0x4000u}} {}

void VuUnits::Host::xgkick(std::span<const std::uint8_t> packet) {
    ++owner_.units_[unit_].stats.xgkicks;
    owner_.gif_.path_data(gs::GifPath::path1, packet);
}

void VuUnits::Host::reserved_instruction(bool upper, std::uint32_t pc, std::uint32_t instruction) {
    if (!reserved) {
        reserved = true;
        reserved_upper = upper;
        reserved_pc = pc;
        reserved_word = instruction;
    }
}

void VuUnits::attach(MachineServices& machine) {
    machine_ = &machine;
    machine.mmio().map(base, size, *this);
    machine.set_vu0(this);
}

void VuUnits::reset() {
    for (Unit& u : units_) {
        std::fill(u.micro.begin(), u.micro.end(), std::uint8_t{0});
        std::fill(u.data.begin(), u.data.end(), std::uint8_t{0});
        u.interp.reset();
        ++u.host.generation;
        u.stats = {};
    }
}

VuUnits::Target VuUnits::locate(std::uint32_t offset) noexcept {
    if (offset < 0x8000u) {
        return {0, offset < 0x4000u, offset & 0xFFFu}; // VU0: 4 KiB, mirrored four times
    }
    return {1, offset < 0xC000u, offset & 0x3FFFu};
}

bool VuUnits::read(std::uint32_t offset, std::uint32_t width, std::uint8_t* out, const MmioContext& /*at*/) {
    const Target t = locate(offset);
    const std::vector<std::uint8_t>& mem = t.micro ? units_[t.unit].micro : units_[t.unit].data;
    for (std::uint32_t i = 0; i < width; ++i) {
        out[i] = mem[(t.offset + i) % mem.size()];
    }
    return true;
}

bool VuUnits::write(std::uint32_t offset, std::uint32_t width, const std::uint8_t* in,
                    const MmioContext& /*at*/) {
    const Target t = locate(offset);
    Unit& u = units_[t.unit];
    std::vector<std::uint8_t>& mem = t.micro ? u.micro : u.data;
    for (std::uint32_t i = 0; i < width; ++i) {
        mem[(t.offset + i) % mem.size()] = in[i];
    }
    if (t.micro) {
        ++u.host.generation;
    }
    return true;
}

void VuUnits::write_micro(unsigned unit, std::uint32_t byte_offset, std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t>& mem = units_[unit].micro;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        mem[(byte_offset + i) % mem.size()] = bytes[i];
    }
    ++units_[unit].host.generation;
}

void VuUnits::start(unsigned unit, std::uint32_t start_pc, std::uint32_t top, std::uint32_t itop) {
    run(unit, true, start_pc, top, itop);
}

void VuUnits::resume(unsigned unit, std::uint32_t top, std::uint32_t itop) {
    run(unit, false, 0, top, itop);
}

std::uint64_t VuUnits::run_from_ee(R5900Context& ctx, std::uint32_t start) {
    VuState& state = units_[0].interp.state();
    load_vu0_state(ctx, state);
    const std::uint64_t before = state.cycles;
    run(0, true, start, 0, ctx.vu0_itop);
    store_vu0_state(state, ctx);
    return state.cycles - before;
}

void VuUnits::run(unsigned unit, bool fresh, std::uint32_t start_pc, std::uint32_t top, std::uint32_t itop) {
    Unit& u = units_[unit];
    const std::uint64_t before = u.interp.state().cycles;
    const auto code_size = static_cast<std::uint32_t>(u.micro.size());
    const auto data_size = static_cast<std::uint32_t>(u.data.size());
    const auto slice = static_cast<std::uint32_t>(std::min<std::uint64_t>(1u << 20, max_program_cycles_));
    if (fresh) {
        u.interp.execute(u.micro.data(), code_size, u.data.data(), data_size, u.host, start_pc, top, itop,
                         slice);
    } else {
        u.interp.resume(u.micro.data(), code_size, u.data.data(), data_size, u.host, top, itop, slice);
    }
    while (u.interp.running() && u.interp.state().cycles - before < max_program_cycles_) {
        u.interp.resume(u.micro.data(), code_size, u.data.data(), data_size, u.host, top, itop, slice);
    }
    ++u.stats.programs;
    u.stats.cycles += u.interp.state().cycles - before;
    if (u.host.reserved) {
        u.host.reserved = false;
        StopInfo info;
        info.reason = StopReason::unsupported;
        info.address = u.host.reserved_pc;
        // The interpreter reports its own limits (a malformed XGKICK packet, a pipeline overflow) with
        // instruction words 0xFFFFFFF6-0xFFFFFFFE.
        info.message = "VU" + std::to_string(unit) + " stopped on a reserved " +
                       (u.host.reserved_upper ? "upper" : "lower") + " instruction " +
                       hex32(u.host.reserved_word) + " at micro address " + hex32(u.host.reserved_pc);
        machine_->stop(std::move(info));
    }
    if (u.interp.running()) {
        StopInfo info;
        info.reason = StopReason::unsupported;
        info.address = u.interp.state().pc;
        info.message = "VU" + std::to_string(unit) + " micro program started at " + hex32(start_pc) +
                       " did not end within " + std::to_string(max_program_cycles_) + " cycles (pc " +
                       hex32(u.interp.state().pc) + ")";
        machine_->stop(std::move(info));
    }
}

} // namespace dq8::rt
