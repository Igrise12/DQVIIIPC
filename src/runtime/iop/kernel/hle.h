// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/iop/cpu/iop_cpu.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The HLE libraries interpreted modules import (Docs/milestones/M4-playable.md, session A): one HleFunction
// per implemented (library, ordinal). Module code reaches one through a linked stub (`j` into the HLE page);
// the executor sets $pc to $ra, charges CostModel::iop_hle_call and calls the function, which reads its
// arguments and either returns a value, blocks the calling thread (its result arrives when it wakes) or hands
// the thread a guest function to run. Arguments follow the o32 convention: $a0-$a3, then the caller's stack
// from $sp + 16.
namespace dq8::rt {

class IopExec;
struct IopThread;
struct HleFunction;

class HleCall {
  public:
    HleCall(IopExec& exec, IopCpuState& cpu, IopThread* thread, const HleFunction& function) noexcept
        : exec_(exec), cpu_(cpu), thread_(thread), function_(function) {}

    std::uint32_t arg(unsigned index);
    std::int32_t sarg(unsigned index) { return static_cast<std::int32_t>(arg(index)); }
    void ret(std::uint32_t value) noexcept { cpu_.gpr[2] = value; }
    void ret(std::int32_t value) noexcept { cpu_.gpr[2] = static_cast<std::uint32_t>(value); }

    IopExec& exec() noexcept { return exec_; }
    IopCpuState& cpu() noexcept { return cpu_; }
    // The calling thread; null in the interrupt context.
    IopThread* thread() noexcept { return thread_; }
    bool in_interrupt() const noexcept { return thread_ == nullptr; }
    const HleFunction& function() const noexcept { return function_; }
    std::uint32_t caller_gp() const noexcept { return cpu_.gpr[28]; }

    // IOP RAM through a guest pointer (any segment or mirror); a pointer outside RAM stops the guest.
    std::uint8_t read8(std::uint32_t address);
    std::uint16_t read16(std::uint32_t address);
    std::uint32_t read32(std::uint32_t address);
    void write8(std::uint32_t address, std::uint8_t value);
    void write16(std::uint32_t address, std::uint16_t value);
    void write32(std::uint32_t address, std::uint32_t value);
    std::vector<std::uint8_t> read_bytes(std::uint32_t address, std::uint32_t size);
    void write_bytes(std::uint32_t address, std::span<const std::uint8_t> bytes);
    // A NUL-terminated string of at most `max` bytes.
    std::string read_string(std::uint32_t address, std::uint32_t max = 1024);
    // The physical RAM offset of [address, address + size); stops when it is not RAM.
    std::uint32_t ram_range(std::uint32_t address, std::uint32_t size);

    // Extra time the function takes (sysclib copies charge per byte).
    void charge(Cycles cycles) noexcept;
    // Stops the guest with a message naming the function and the caller.
    [[noreturn]] void fail(const std::string& what);

  private:
    IopExec& exec_;
    IopCpuState& cpu_;
    IopThread* thread_;
    const HleFunction& function_;
};

using HleFn = void (*)(HleCall&);

struct HleFunction {
    std::string_view library;
    std::uint16_t ordinal = 0;
    std::string_view name;
    HleFn fn = nullptr;
};

// Every HLE function, grouped by library in ordinal order.
std::span<const HleFunction> hle_functions();
// The imports of the ten sound modules (the census in Docs/milestones/M4-playable.md): (library, ordinal).
std::span<const std::pair<std::string_view, std::uint16_t>> sound_module_census();

// The per-library tables (hle_*.cpp).
std::span<const HleFunction> hle_intrman();
std::span<const HleFunction> hle_loadcore();
std::span<const HleFunction> hle_sysmem();
std::span<const HleFunction> hle_sysclib();
std::span<const HleFunction> hle_stdio();
std::span<const HleFunction> hle_thbase();
std::span<const HleFunction> hle_thsemap();
std::span<const HleFunction> hle_thevent();
std::span<const HleFunction> hle_timrman();
std::span<const HleFunction> hle_sifman();
std::span<const HleFunction> hle_sifcmd();

} // namespace dq8::rt
