// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xIOP/src/emulator/core/iop_cpu.cpp (IopCpuCore::executeInstruction). Changes: the HLE trap fetch, faults
// that end the run instead of vectoring, COP0 treated as reserved, LWL/LWR merging with a pending load.
#include "runtime/iop/cpu/iop_cpu.h"

#include <cstring>
#include <limits>

namespace dq8::rt {

namespace {

constexpr std::uint32_t ram_mask = iop_ram_bytes - 1;

std::uint32_t sign16(std::uint32_t imm) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(imm & 0xFFFFu)));
}

} // namespace

std::uint32_t iop_exception_code(IopFault fault) noexcept {
    switch (fault) {
    case IopFault::fetch_address:
    case IopFault::load_address:
        return 4; // AdEL
    case IopFault::store_address:
        return 5; // AdES
    case IopFault::fetch_bus:
        return 6; // IBE
    case IopFault::load_bus:
    case IopFault::store_bus:
        return 7; // DBE
    case IopFault::syscall:
        return 8;
    case IopFault::break_:
        return 9;
    case IopFault::reserved:
        return 10;
    case IopFault::overflow:
        return 12;
    case IopFault::none:
        break;
    }
    return 0;
}

const char* to_string(IopFault fault) noexcept {
    switch (fault) {
    case IopFault::none:
        return "none";
    case IopFault::fetch_address:
        return "a misaligned instruction fetch";
    case IopFault::fetch_bus:
        return "an instruction fetch outside RAM";
    case IopFault::load_address:
        return "a misaligned load";
    case IopFault::store_address:
        return "a misaligned store";
    case IopFault::load_bus:
        return "a load from an unmapped address";
    case IopFault::store_bus:
        return "a store to an unmapped address";
    case IopFault::syscall:
        return "a syscall instruction";
    case IopFault::break_:
        return "a break instruction";
    case IopFault::reserved:
        return "a reserved instruction";
    case IopFault::overflow:
        return "an arithmetic overflow";
    }
    return "?";
}

bool IopCpu::load(std::uint32_t address, std::uint32_t width, std::uint32_t& value, std::uint32_t pc) {
    const std::uint32_t phys = iop_physical(address);
    if (phys < iop_ram_mirror_end) {
        const std::uint8_t* p = ram_ + (phys & ram_mask);
        switch (width) {
        case 1:
            value = *p;
            break;
        case 2: {
            std::uint16_t h = 0;
            std::memcpy(&h, p, 2);
            value = h;
            break;
        }
        default:
            std::memcpy(&value, p, 4);
            break;
        }
        return true;
    }
    return bus_.read(phys, width, value, pc);
}

bool IopCpu::store(std::uint32_t address, std::uint32_t width, std::uint32_t value, std::uint32_t pc) {
    const std::uint32_t phys = iop_physical(address);
    if (phys < iop_ram_mirror_end) {
        std::uint8_t* p = ram_ + (phys & ram_mask);
        switch (width) {
        case 1:
            *p = static_cast<std::uint8_t>(value);
            break;
        case 2: {
            const auto h = static_cast<std::uint16_t>(value);
            std::memcpy(p, &h, 2);
            break;
        }
        default:
            std::memcpy(p, &value, 4);
            break;
        }
        return true;
    }
    return bus_.write(phys, width, value, pc);
}

IopCpuExit IopCpu::run(IopCpuState& state, Cycles& now, const Cycles& limit, Cycles per_instruction,
                       IopCpuStop& stop) {
    while (now < limit) {
        const Step r = step(state, stop);
        if (r == Step::trap) {
            return IopCpuExit::trap;
        }
        ++instructions_;
        now += per_instruction;
        if (r == Step::fault) {
            return IopCpuExit::fault;
        }
    }
    return IopCpuExit::limit;
}

IopCpu::Step IopCpu::step(IopCpuState& s, IopCpuStop& stop) {
    const std::uint32_t pc = s.pc;
    const bool in_delay_slot = s.branch_pending;
    auto fault = [&](IopFault f, std::uint32_t address, std::uint32_t instruction) {
        stop.fault = f;
        stop.pc = pc;
        stop.address = address;
        stop.instruction = instruction;
        stop.delay_slot = in_delay_slot;
        return Step::fault;
    };

    if ((pc & 3u) != 0) {
        return fault(IopFault::fetch_address, pc, 0);
    }
    const std::uint32_t phys_pc = iop_physical(pc);
    std::uint32_t instruction = 0;
    if (phys_pc < iop_ram_mirror_end) {
        std::memcpy(&instruction, ram_ + (phys_pc & ram_mask), 4);
    } else {
        const auto slot = bus_.hle_slot(phys_pc);
        if (!slot || in_delay_slot) {
            return fault(IopFault::fetch_bus, pc, 0);
        }
        s.retire_load();
        stop.fault = IopFault::none;
        stop.pc = pc;
        stop.trap = *slot;
        return Step::trap;
    }

    const std::uint32_t delay_target = s.branch_target;
    s.branch_pending = false;

    const std::uint32_t opcode = instruction >> 26;
    const std::uint32_t rs = (instruction >> 21) & 31u;
    const std::uint32_t rt = (instruction >> 16) & 31u;
    const std::uint32_t rd = (instruction >> 11) & 31u;
    const std::uint32_t sa = (instruction >> 6) & 31u;
    const std::uint32_t funct = instruction & 63u;
    const std::uint32_t imm = instruction & 0xFFFFu;
    const std::uint32_t simm = sign16(imm);
    const std::uint32_t next_pc = pc + 4;
    auto& r = s.gpr;

    std::uint32_t written = 0; // the register this instruction wrote directly (it beats a pending load)
    bool new_load = false;
    std::uint32_t new_load_reg = 0;
    std::uint32_t new_load_value = 0;
    bool new_branch = false;
    std::uint32_t new_target = 0;

    auto write = [&](std::uint32_t reg, std::uint32_t value) {
        if (reg != 0) {
            r[reg] = value;
            written = reg;
        }
    };
    auto delayed = [&](std::uint32_t reg, std::uint32_t value) {
        if (reg != 0) {
            new_load = true;
            new_load_reg = reg;
            new_load_value = value;
        }
    };
    auto branch = [&](bool taken) {
        if (taken) {
            new_branch = true;
            new_target = next_pc + (simm << 2);
        }
    };
    // The value LWL/LWR merge with: a load still in flight to the same register is forwarded.
    auto merge_base = [&](std::uint32_t reg) {
        return s.load_pending && s.load_reg == reg ? s.load_value : r[reg];
    };

    switch (opcode) {
    case 0x00: // SPECIAL
        switch (funct) {
        case 0x00: // SLL
            write(rd, r[rt] << sa);
            break;
        case 0x02: // SRL
            write(rd, r[rt] >> sa);
            break;
        case 0x03: // SRA
            write(rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(r[rt]) >> sa));
            break;
        case 0x04: // SLLV
            write(rd, r[rt] << (r[rs] & 31u));
            break;
        case 0x06: // SRLV
            write(rd, r[rt] >> (r[rs] & 31u));
            break;
        case 0x07: // SRAV
            write(rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(r[rt]) >> (r[rs] & 31u)));
            break;
        case 0x08: // JR
            new_branch = true;
            new_target = r[rs];
            break;
        case 0x09: { // JALR
            const std::uint32_t target = r[rs];
            write(rd, pc + 8);
            new_branch = true;
            new_target = target;
            break;
        }
        case 0x0C:
            return fault(IopFault::syscall, 0, instruction);
        case 0x0D:
            return fault(IopFault::break_, 0, instruction);
        case 0x10: // MFHI
            write(rd, s.hi);
            break;
        case 0x11: // MTHI
            s.hi = r[rs];
            break;
        case 0x12: // MFLO
            write(rd, s.lo);
            break;
        case 0x13: // MTLO
            s.lo = r[rs];
            break;
        case 0x18: { // MULT
            const std::int64_t p = static_cast<std::int64_t>(static_cast<std::int32_t>(r[rs])) *
                                   static_cast<std::int64_t>(static_cast<std::int32_t>(r[rt]));
            s.lo = static_cast<std::uint32_t>(p);
            s.hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(p) >> 32);
            break;
        }
        case 0x19: { // MULTU
            const std::uint64_t p = static_cast<std::uint64_t>(r[rs]) * r[rt];
            s.lo = static_cast<std::uint32_t>(p);
            s.hi = static_cast<std::uint32_t>(p >> 32);
            break;
        }
        case 0x1A: { // DIV
            const auto n = static_cast<std::int32_t>(r[rs]);
            const auto d = static_cast<std::int32_t>(r[rt]);
            if (d == 0) {
                s.lo = n >= 0 ? 0xFFFFFFFFu : 1u;
                s.hi = static_cast<std::uint32_t>(n);
            } else if (n == std::numeric_limits<std::int32_t>::min() && d == -1) {
                s.lo = static_cast<std::uint32_t>(n);
                s.hi = 0;
            } else {
                s.lo = static_cast<std::uint32_t>(n / d);
                s.hi = static_cast<std::uint32_t>(n % d);
            }
            break;
        }
        case 0x1B: // DIVU
            if (r[rt] == 0) {
                s.lo = 0xFFFFFFFFu;
                s.hi = r[rs];
            } else {
                s.lo = r[rs] / r[rt];
                s.hi = r[rs] % r[rt];
            }
            break;
        case 0x20: { // ADD
            const std::int64_t v = static_cast<std::int64_t>(static_cast<std::int32_t>(r[rs])) +
                                   static_cast<std::int32_t>(r[rt]);
            if (v > std::numeric_limits<std::int32_t>::max() ||
                v < std::numeric_limits<std::int32_t>::min()) {
                return fault(IopFault::overflow, 0, instruction);
            }
            write(rd, static_cast<std::uint32_t>(v));
            break;
        }
        case 0x21: // ADDU
            write(rd, r[rs] + r[rt]);
            break;
        case 0x22: { // SUB
            const std::int64_t v = static_cast<std::int64_t>(static_cast<std::int32_t>(r[rs])) -
                                   static_cast<std::int32_t>(r[rt]);
            if (v > std::numeric_limits<std::int32_t>::max() ||
                v < std::numeric_limits<std::int32_t>::min()) {
                return fault(IopFault::overflow, 0, instruction);
            }
            write(rd, static_cast<std::uint32_t>(v));
            break;
        }
        case 0x23: // SUBU
            write(rd, r[rs] - r[rt]);
            break;
        case 0x24: // AND
            write(rd, r[rs] & r[rt]);
            break;
        case 0x25: // OR
            write(rd, r[rs] | r[rt]);
            break;
        case 0x26: // XOR
            write(rd, r[rs] ^ r[rt]);
            break;
        case 0x27: // NOR
            write(rd, ~(r[rs] | r[rt]));
            break;
        case 0x2A: // SLT
            write(rd, static_cast<std::int32_t>(r[rs]) < static_cast<std::int32_t>(r[rt]) ? 1u : 0u);
            break;
        case 0x2B: // SLTU
            write(rd, r[rs] < r[rt] ? 1u : 0u);
            break;
        default:
            return fault(IopFault::reserved, 0, instruction);
        }
        break;
    case 0x01: // REGIMM
        switch (rt) {
        case 0x00: // BLTZ
            branch(static_cast<std::int32_t>(r[rs]) < 0);
            break;
        case 0x01: // BGEZ
            branch(static_cast<std::int32_t>(r[rs]) >= 0);
            break;
        case 0x10: { // BLTZAL
            const bool taken = static_cast<std::int32_t>(r[rs]) < 0;
            write(31, pc + 8);
            branch(taken);
            break;
        }
        case 0x11: { // BGEZAL
            const bool taken = static_cast<std::int32_t>(r[rs]) >= 0;
            write(31, pc + 8);
            branch(taken);
            break;
        }
        default:
            return fault(IopFault::reserved, 0, instruction);
        }
        break;
    case 0x02: // J
        new_branch = true;
        new_target = (next_pc & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2);
        break;
    case 0x03: // JAL
        write(31, pc + 8);
        new_branch = true;
        new_target = (next_pc & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2);
        break;
    case 0x04: // BEQ
        branch(r[rs] == r[rt]);
        break;
    case 0x05: // BNE
        branch(r[rs] != r[rt]);
        break;
    case 0x06: // BLEZ
        branch(static_cast<std::int32_t>(r[rs]) <= 0);
        break;
    case 0x07: // BGTZ
        branch(static_cast<std::int32_t>(r[rs]) > 0);
        break;
    case 0x08: { // ADDI
        const std::int64_t v =
            static_cast<std::int64_t>(static_cast<std::int32_t>(r[rs])) + static_cast<std::int32_t>(simm);
        if (v > std::numeric_limits<std::int32_t>::max() || v < std::numeric_limits<std::int32_t>::min()) {
            return fault(IopFault::overflow, 0, instruction);
        }
        write(rt, static_cast<std::uint32_t>(v));
        break;
    }
    case 0x09: // ADDIU
        write(rt, r[rs] + simm);
        break;
    case 0x0A: // SLTI
        write(rt, static_cast<std::int32_t>(r[rs]) < static_cast<std::int32_t>(simm) ? 1u : 0u);
        break;
    case 0x0B: // SLTIU
        write(rt, r[rs] < simm ? 1u : 0u);
        break;
    case 0x0C: // ANDI
        write(rt, r[rs] & imm);
        break;
    case 0x0D: // ORI
        write(rt, r[rs] | imm);
        break;
    case 0x0E: // XORI
        write(rt, r[rs] ^ imm);
        break;
    case 0x0F: // LUI
        write(rt, imm << 16);
        break;
    case 0x20:   // LB
    case 0x24: { // LBU
        const std::uint32_t address = r[rs] + simm;
        std::uint32_t v = 0;
        if (!load(address, 1, v, pc)) {
            return fault(IopFault::load_bus, address, instruction);
        }
        delayed(rt, opcode == 0x20 ? static_cast<std::uint32_t>(static_cast<std::int8_t>(v)) : v);
        break;
    }
    case 0x21:   // LH
    case 0x25: { // LHU
        const std::uint32_t address = r[rs] + simm;
        if ((address & 1u) != 0) {
            return fault(IopFault::load_address, address, instruction);
        }
        std::uint32_t v = 0;
        if (!load(address, 2, v, pc)) {
            return fault(IopFault::load_bus, address, instruction);
        }
        delayed(rt, opcode == 0x21 ? static_cast<std::uint32_t>(static_cast<std::int16_t>(v)) : v);
        break;
    }
    case 0x22: { // LWL
        const std::uint32_t address = r[rs] + simm;
        std::uint32_t word = 0;
        if (!load(address & ~3u, 4, word, pc)) {
            return fault(IopFault::load_bus, address, instruction);
        }
        static constexpr std::uint32_t keep[4] = {0x00FFFFFFu, 0x0000FFFFu, 0x000000FFu, 0x00000000u};
        static constexpr std::uint32_t shift[4] = {24, 16, 8, 0};
        const std::uint32_t k = address & 3u;
        delayed(rt, (merge_base(rt) & keep[k]) | (word << shift[k]));
        break;
    }
    case 0x23: { // LW
        const std::uint32_t address = r[rs] + simm;
        if ((address & 3u) != 0) {
            return fault(IopFault::load_address, address, instruction);
        }
        std::uint32_t v = 0;
        if (!load(address, 4, v, pc)) {
            return fault(IopFault::load_bus, address, instruction);
        }
        delayed(rt, v);
        break;
    }
    case 0x26: { // LWR
        const std::uint32_t address = r[rs] + simm;
        std::uint32_t word = 0;
        if (!load(address & ~3u, 4, word, pc)) {
            return fault(IopFault::load_bus, address, instruction);
        }
        static constexpr std::uint32_t keep[4] = {0x00000000u, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u};
        static constexpr std::uint32_t shift[4] = {0, 8, 16, 24};
        const std::uint32_t k = address & 3u;
        delayed(rt, (merge_base(rt) & keep[k]) | (word >> shift[k]));
        break;
    }
    case 0x28: { // SB
        const std::uint32_t address = r[rs] + simm;
        if (!store(address, 1, r[rt] & 0xFFu, pc)) {
            return fault(IopFault::store_bus, address, instruction);
        }
        break;
    }
    case 0x29: { // SH
        const std::uint32_t address = r[rs] + simm;
        if ((address & 1u) != 0) {
            return fault(IopFault::store_address, address, instruction);
        }
        if (!store(address, 2, r[rt] & 0xFFFFu, pc)) {
            return fault(IopFault::store_bus, address, instruction);
        }
        break;
    }
    case 0x2A: { // SWL
        const std::uint32_t address = r[rs] + simm;
        std::uint32_t old = 0;
        if (!load(address & ~3u, 4, old, pc)) {
            return fault(IopFault::store_bus, address, instruction);
        }
        const std::uint32_t v = r[rt];
        std::uint32_t merged = old;
        switch (address & 3u) {
        case 0:
            merged = (old & 0xFFFFFF00u) | (v >> 24);
            break;
        case 1:
            merged = (old & 0xFFFF0000u) | (v >> 16);
            break;
        case 2:
            merged = (old & 0xFF000000u) | (v >> 8);
            break;
        default:
            merged = v;
            break;
        }
        if (!store(address & ~3u, 4, merged, pc)) {
            return fault(IopFault::store_bus, address, instruction);
        }
        break;
    }
    case 0x2B: { // SW
        const std::uint32_t address = r[rs] + simm;
        if ((address & 3u) != 0) {
            return fault(IopFault::store_address, address, instruction);
        }
        if (!store(address, 4, r[rt], pc)) {
            return fault(IopFault::store_bus, address, instruction);
        }
        break;
    }
    case 0x2E: { // SWR
        const std::uint32_t address = r[rs] + simm;
        std::uint32_t old = 0;
        if (!load(address & ~3u, 4, old, pc)) {
            return fault(IopFault::store_bus, address, instruction);
        }
        const std::uint32_t v = r[rt];
        std::uint32_t merged = old;
        switch (address & 3u) {
        case 0:
            merged = v;
            break;
        case 1:
            merged = (old & 0x000000FFu) | (v << 8);
            break;
        case 2:
            merged = (old & 0x0000FFFFu) | (v << 16);
            break;
        default:
            merged = (old & 0x00FFFFFFu) | (v << 24);
            break;
        }
        if (!store(address & ~3u, 4, merged, pc)) {
            return fault(IopFault::store_bus, address, instruction);
        }
        break;
    }
    default: // COP0, COP2, LWC/SWC and the encodings MIPS I leaves undefined
        return fault(IopFault::reserved, 0, instruction);
    }

    // The load issued by the previous instruction lands now, unless this instruction wrote that register.
    if (s.load_pending) {
        if (s.load_reg != written) {
            r[s.load_reg] = s.load_value;
        }
        s.load_pending = false;
    }
    if (new_load) {
        s.load_pending = true;
        s.load_reg = new_load_reg;
        s.load_value = new_load_value;
    }
    r[0] = 0;
    if (in_delay_slot) {
        s.pc = delay_target; // a branch in a delay slot is ignored, as upstream does
    } else {
        s.pc = next_pc;
        s.branch_pending = new_branch;
        s.branch_target = new_target;
    }
    return Step::next;
}

} // namespace dq8::rt
