// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xIOP/src/emulator/core/iop_cpu.h.
#pragma once

#include "runtime/machine/clock.h"

#include <array>
#include <bit>
#include <cstdint>
#include <optional>

// The R3000A as the interpreted IOP runs it (Docs/milestones/M4-playable.md, session A; ADR-0014): user-mode
// MIPS I integer code with the branch-delay and load-delay slots. There is no COP0, no exception vector and
// no cache: an instruction the sound modules never use (COP0, COP2, syscall) or a fault (address error, bus
// error, break, overflow) ends the run with an IopCpuStop the caller turns into a guest stop. An instruction
// fetch from the HLE page ends the run as a trap: the caller performs the HLE call and resumes.
namespace dq8::rt {

static_assert(std::endian::native == std::endian::little, "IOP RAM is accessed as host words");

// IOP RAM is 2 MiB, mirrored four times in the first 8 MiB of the physical address space.
inline constexpr std::uint32_t iop_ram_bytes = 2u << 20;
inline constexpr std::uint32_t iop_ram_mirror_end = 8u << 20;

// The physical address an IOP virtual address stands for (kuseg, kseg0 and kseg1 are the same memory).
constexpr std::uint32_t iop_physical(std::uint32_t address) noexcept {
    return address & 0x1FFFFFFFu;
}

// One execution context's registers: a thread's, or the interrupt context's.
struct IopCpuState {
    std::array<std::uint32_t, 32> gpr{};
    std::uint32_t hi = 0;
    std::uint32_t lo = 0;
    std::uint32_t pc = 0;
    // A taken branch or jump whose delay slot has not executed yet.
    bool branch_pending = false;
    std::uint32_t branch_target = 0;
    // A load whose value lands after the next instruction (the load-delay slot).
    bool load_pending = false;
    std::uint32_t load_reg = 0;
    std::uint32_t load_value = 0;

    // Lands a pending load now (before an HLE call reads the argument registers).
    void retire_load() noexcept {
        if (load_pending && load_reg != 0) {
            gpr[load_reg] = load_value;
        }
        load_pending = false;
    }
};

// Why a run ended early.
enum class IopFault : std::uint8_t {
    none,
    fetch_address, // pc not word aligned
    fetch_bus,     // pc outside RAM and the HLE page
    load_address,  // misaligned halfword or word load
    store_address, // misaligned halfword or word store
    load_bus,      // a load the bus refused
    store_bus,     // a store the bus refused
    syscall,
    break_,
    reserved, // an instruction outside user-mode MIPS I (COP0, COP2, ...) or an undefined encoding
    overflow, // add, addi or sub overflowed
};

// MIPS exception codes, for StopInfo::number.
std::uint32_t iop_exception_code(IopFault fault) noexcept;
const char* to_string(IopFault fault) noexcept;

enum class IopCpuExit : std::uint8_t { limit, trap, fault };

struct IopCpuStop {
    IopFault fault = IopFault::none;
    std::uint32_t pc = 0;          // the faulting instruction, or the trap address
    std::uint32_t address = 0;     // the data address of a load or store fault
    std::uint32_t instruction = 0; // the faulting instruction word
    bool delay_slot = false;       // the faulting instruction was in a branch-delay slot
    std::uint32_t trap = 0;        // the HLE slot index of a trap
};

// What the core needs beyond RAM. RAM (and its mirrors) is read and written directly; every other physical
// address goes here. A refused access is a bus error.
class IIopCpuBus {
  public:
    virtual ~IIopCpuBus() = default;
    // `width` is 1, 2 or 4; the address is aligned to it.
    virtual bool read(std::uint32_t physical, std::uint32_t width, std::uint32_t& value,
                      std::uint32_t pc) = 0;
    virtual bool write(std::uint32_t physical, std::uint32_t width, std::uint32_t value,
                       std::uint32_t pc) = 0;
    // The HLE slot an instruction fetch outside RAM lands on, if any.
    virtual std::optional<std::uint32_t> hle_slot(std::uint32_t physical) const = 0;
};

class IopCpu {
  public:
    // `ram` holds iop_ram_bytes bytes and outlives the core.
    IopCpu(std::uint8_t* ram, IIopCpuBus& bus) noexcept : ram_(ram), bus_(bus) {}

    // Executes instructions while now < limit, adding `per_instruction` to `now` for each. `limit` may be
    // lowered by the bus during the run (an interrupt raised, an earlier event scheduled). Ends at the limit,
    // at an HLE trap (state.pc is the trap address, `stop.trap` its slot) or at a fault (state.pc is the
    // faulting instruction).
    IopCpuExit run(IopCpuState& state, Cycles& now, const Cycles& limit, Cycles per_instruction,
                   IopCpuStop& stop);

    std::uint64_t instructions() const noexcept { return instructions_; }

  private:
    enum class Step : std::uint8_t { next, trap, fault };
    Step step(IopCpuState& s, IopCpuStop& stop);
    bool load(std::uint32_t address, std::uint32_t width, std::uint32_t& value, std::uint32_t pc);
    bool store(std::uint32_t address, std::uint32_t width, std::uint32_t value, std::uint32_t pc);

    std::uint8_t* ram_;
    IIopCpuBus& bus_;
    std::uint64_t instructions_ = 0;
};

} // namespace dq8::rt
