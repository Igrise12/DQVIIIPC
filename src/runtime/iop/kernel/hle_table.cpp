// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/iop_exec.h"
#include "runtime/iop/iop_memory.h"
#include "runtime/iop/kernel/hle.h"

#include <array>
#include <cstring>

namespace dq8::rt {

namespace {

// The census of Docs/milestones/M4-playable.md: every (library, ordinal) the ten sound modules import from
// the IOP kernel. Names and ordinals only.
constexpr std::pair<std::string_view, std::uint16_t> census[] = {
    {"intrman", 4},  {"intrman", 5},  {"intrman", 6},  {"intrman", 7},  {"intrman", 17}, {"intrman", 18},
    {"intrman", 23}, {"loadcore", 5}, {"loadcore", 6}, {"loadcore", 7}, {"sifcmd", 14},  {"sifcmd", 15},
    {"sifcmd", 16},  {"sifcmd", 17},  {"sifcmd", 19},  {"sifcmd", 22},  {"sifcmd", 24},  {"sifcmd", 25},
    {"sifman", 7},   {"sifman", 8},   {"stdio", 4},    {"sysclib", 8},  {"sysclib", 12}, {"sysclib", 14},
    {"sysclib", 17}, {"sysclib", 27}, {"sysclib", 29}, {"sysclib", 36}, {"sysmem", 4},   {"sysmem", 5},
    {"sysmem", 14},  {"thbase", 4},   {"thbase", 5},   {"thbase", 6},   {"thbase", 10},  {"thbase", 14},
    {"thbase", 20},  {"thbase", 22},  {"thbase", 24},  {"thbase", 26},  {"thbase", 28},  {"thbase", 33},
    {"thbase", 34},  {"thbase", 36},  {"thbase", 37},  {"thbase", 38},  {"thbase", 39},  {"thbase", 40},
    {"thevent", 4},  {"thevent", 5},  {"thevent", 6},  {"thevent", 7},  {"thevent", 8},  {"thevent", 9},
    {"thevent", 10}, {"thsemap", 4},  {"thsemap", 5},  {"thsemap", 6},  {"thsemap", 7},  {"thsemap", 8},
    {"timrman", 4},  {"timrman", 6},  {"timrman", 20}, {"timrman", 22}, {"timrman", 23}, {"timrman", 24},
};
static_assert(std::size(census) == 66);

std::string where(HleCall& call) {
    const std::uint32_t caller = call.cpu().gpr[31] - 8;
    return (call.thread() != nullptr ? "IOP thread " + std::to_string(call.thread()->id)
                                     : std::string("the IOP interrupt context")) +
           ", called from " + call.exec().location(caller);
}

} // namespace

std::span<const std::pair<std::string_view, std::uint16_t>> sound_module_census() {
    return census;
}

std::span<const HleFunction> hle_functions() {
    static const std::vector<HleFunction> all = [] {
        std::vector<HleFunction> v;
        for (const auto lib :
             {hle_intrman(), hle_loadcore(), hle_sysmem(), hle_sysclib(), hle_stdio(), hle_thbase(),
              hle_thsemap(), hle_thevent(), hle_timrman(), hle_sifman(), hle_sifcmd()}) {
            v.insert(v.end(), lib.begin(), lib.end());
        }
        return v;
    }();
    return all;
}

std::uint32_t HleCall::arg(unsigned index) {
    if (index < 4) {
        return cpu_.gpr[4 + index];
    }
    return read32(cpu_.gpr[29] + 4 * index);
}

std::uint32_t HleCall::ram_range(std::uint32_t address, std::uint32_t size) {
    const std::uint32_t phys = iop_physical(address);
    if (phys >= iop_ram_mirror_end) {
        fail("the pointer " + hex32(address) + " is not IOP RAM");
    }
    const std::uint32_t at = phys & (iop_ram_bytes - 1);
    if (size > iop_ram_bytes - at) {
        fail(std::to_string(size) + " bytes at " + hex32(address) + " run past IOP RAM");
    }
    return at;
}

std::uint8_t HleCall::read8(std::uint32_t address) {
    return exec_.ram().data()[ram_range(address, 1)];
}

std::uint16_t HleCall::read16(std::uint32_t address) {
    std::uint16_t v = 0;
    std::memcpy(&v, exec_.ram().data() + ram_range(address, 2), 2);
    return v;
}

std::uint32_t HleCall::read32(std::uint32_t address) {
    std::uint32_t v = 0;
    std::memcpy(&v, exec_.ram().data() + ram_range(address, 4), 4);
    return v;
}

void HleCall::write8(std::uint32_t address, std::uint8_t value) {
    exec_.ram().data()[ram_range(address, 1)] = value;
}

void HleCall::write16(std::uint32_t address, std::uint16_t value) {
    std::memcpy(exec_.ram().data() + ram_range(address, 2), &value, 2);
}

void HleCall::write32(std::uint32_t address, std::uint32_t value) {
    std::memcpy(exec_.ram().data() + ram_range(address, 4), &value, 4);
}

std::vector<std::uint8_t> HleCall::read_bytes(std::uint32_t address, std::uint32_t size) {
    std::vector<std::uint8_t> out(size);
    if (size != 0) {
        std::memcpy(out.data(), exec_.ram().data() + ram_range(address, size), size);
    }
    return out;
}

void HleCall::write_bytes(std::uint32_t address, std::span<const std::uint8_t> bytes) {
    if (!bytes.empty()) {
        std::memcpy(exec_.ram().data() + ram_range(address, static_cast<std::uint32_t>(bytes.size())),
                    bytes.data(), bytes.size());
    }
}

std::string HleCall::read_string(std::uint32_t address, std::uint32_t max) {
    std::string out;
    for (std::uint32_t i = 0; i < max; ++i) {
        const std::uint8_t c = read8(address + i);
        if (c == 0) {
            break;
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

void HleCall::charge(Cycles cycles) noexcept {
    exec_.charge(cycles);
}

void HleCall::fail(const std::string& what) {
    exec_.stop(StopReason::guest_error,
               std::string(function_.library) + " " + std::string(function_.name) + ": " + what + " (" +
                   where(*this) + ")",
               cpu_.gpr[31] - 8);
}

} // namespace dq8::rt
