// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// M4 day-0 stub of the SPU2 (spu2.h); M4 session B replaces it.
#include "runtime/spu2/spu2.h"

#include "runtime/ee/guest_stop.h"

#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace dq8::rt {

Spu2::Spu2(Spu2Config config) : config_(config) {}

void Spu2::attach(MachineServices& machine) {
    machine_ = &machine;
}

void Spu2::reset() {}

void Spu2::connect(IIopHardwareHost& host) {
    host_ = &host;
}

bool Spu2::read(std::uint32_t offset, std::uint32_t /*width*/, std::uint32_t& /*value*/,
                const IopAccess& /*at*/) {
    not_implemented("register read at offset " + hex32(offset));
}

bool Spu2::write(std::uint32_t offset, std::uint32_t /*width*/, std::uint32_t /*value*/,
                 const IopAccess& /*at*/) {
    not_implemented("register write at offset " + hex32(offset));
}

IopDmaStart Spu2::dma_start(const IopDmaRequest& request) {
    not_implemented("DMA on IOP channel " + std::to_string(request.channel));
}

void Spu2::dma_stop(std::uint32_t channel, Cycles /*at*/) {
    not_implemented("DMA stop on IOP channel " + std::to_string(channel));
}

void Spu2::advance_to(Cycles /*t*/) {}

IopDeviceMapping Spu2::mapping() {
    IopDeviceMapping m;
    m.base = base;
    m.size = size;
    m.device = this;
    m.client = this;
    m.dma = this;
    m.dma_channels = {4, 7};
    return m;
}

std::string Spu2::ram_sha1() const {
    return {};
}

void Spu2::not_implemented(const std::string& what) const {
    StopInfo info;
    info.reason = StopReason::unsupported;
    info.message = "spu2: " + what + " is not implemented (M4 session B)";
    if (machine_ == nullptr) {
        throw std::logic_error(info.message);
    }
    machine_->stop(std::move(info));
    std::abort(); // stop() never returns; GCC cannot see that through the virtual call
}

} // namespace dq8::rt
