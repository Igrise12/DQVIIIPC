// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/movie/ipu_regs.h"

#include "runtime/ee/guest_stop.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace dq8::rt {

namespace {

constexpr std::uint32_t reg_cmd = 0x00;
constexpr std::uint32_t reg_ctrl = 0x10;
constexpr std::uint32_t regs_size = 0x40; // CMD, CTRL, BP, TOP
constexpr std::uint32_t fifo_size = 0x20; // OUT, IN
constexpr std::uint32_t fifo_out = 0x00;
constexpr std::uint32_t fifo_in = 0x10;

// IPU command codes (CMD bits 31:28): the set-up commands the player's init issues, and the decode commands
// only the replaced sceMpeg path would.
constexpr std::uint32_t cmd_bclr = 0x0, cmd_setiq = 0x5, cmd_setvq = 0x6, cmd_setth = 0x9;

std::uint32_t load32(const std::uint8_t* in, std::uint32_t width) {
    std::uint32_t v = 0;
    std::memcpy(&v, in, width < 4 ? width : 4);
    return v;
}

const char* command_name(std::uint32_t code) {
    static const char* const names[16] = {"BCLR", "IDEC",  "BDEC", "VDEC", "FDEC", "SETIQ", "SETVQ", "CSC",
                                          "PACK", "SETTH", "0xA",  "0xB",  "0xC",  "0xD",   "0xE",   "0xF"};
    return names[code & 0xFu];
}

} // namespace

void IpuRegisters::attach(MachineServices& machine) {
    machine_ = &machine;
    machine.mmio().map(regs_base, regs_size, *this);
    machine.mmio().map(fifo_base, fifo_size, fifo_);
}

void IpuRegisters::reset() {
    commands_ = 0;
    fifo_writes_ = 0;
    thresholds_ = IpuThresholds{};
}

IpuRegisters* IpuRegisters::find(MachineServices& machine) noexcept {
    const auto hit = machine.mmio().find(regs_base);
    return hit ? dynamic_cast<IpuRegisters*>(hit->device) : nullptr;
}

bool IpuRegisters::read(std::uint32_t /*offset*/, std::uint32_t width, std::uint8_t* out,
                        const MmioContext& /*at*/) {
    std::memset(out, 0, width); // idle: no command busy, no data, no error
    return true;
}

bool IpuRegisters::write(std::uint32_t offset, std::uint32_t width, const std::uint8_t* in,
                         const MmioContext& at) {
    if (offset == reg_cmd) {
        const std::uint32_t value = load32(in, width);
        const std::uint32_t code = value >> 28;
        if (code != cmd_bclr && code != cmd_setiq && code != cmd_setvq && code != cmd_setth) {
            decode_path(std::string("IPU command ") + command_name(code), value, at);
        }
        if (code == cmd_setth) {
            thresholds_.th0 = static_cast<std::uint16_t>(value & 0x1FFu);
            thresholds_.th1 = static_cast<std::uint16_t>((value >> 16) & 0x1FFu);
        }
        ++commands_;
        return true;
    }
    return offset == reg_ctrl || offset == 0x20 || offset == 0x30; // CTRL (reset) and the read-only BP/TOP
}

bool IpuRegisters::Fifo::read(std::uint32_t offset, std::uint32_t width, std::uint8_t* out,
                              const MmioContext& at) {
    if (offset == fifo_out) {
        owner_.decode_path("an IPU OUT FIFO read", 0, at);
    }
    std::memset(out, 0, width);
    return true;
}

bool IpuRegisters::Fifo::write(std::uint32_t offset, std::uint32_t /*width*/, const std::uint8_t* /*in*/,
                               const MmioContext& /*at*/) {
    if (offset >= fifo_in) {
        ++owner_.fifo_writes_; // the set-up commands' table data; discarded
        return true;
    }
    return false;
}

void IpuRegisters::decode_path(std::string_view what, std::uint32_t value, const MmioContext& at) const {
    StopInfo info;
    info.reason = StopReason::unsupported;
    info.pc = at.pc;
    info.address = value;
    info.message =
        std::string(what) + " (" + hex32(value) + ") at pc " + hex32(at.pc) +
        ": the IPU decodes only through the sceMpeg HLE (M4 session C), which did not cover this path";
    machine_->stop(std::move(info));
    std::abort(); // stop() never returns; GCC cannot see that through the virtual call
}

} // namespace dq8::rt
