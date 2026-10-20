// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/context.h"

#include <cstdio>
#include <cstring>

namespace dq8::rt {

const std::array<std::string_view, 32> gpr_names{
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
};

namespace {

constexpr unsigned reg_sp = 29;
constexpr unsigned reg_a0 = 4;
constexpr unsigned reg_a1 = 5;
constexpr unsigned reg_ra = 31;

void read_gpr(const R5900Context& ctx, unsigned reg, std::uint64_t out[2]) noexcept {
    std::memcpy(out, &ctx.r[reg & 31u], 16);
}

} // namespace

void reset_context(R5900Context& ctx, std::uint32_t entry) {
    ctx = R5900Context();
    ctx.pc = entry;
    set_gpr_lo(ctx, reg_sp, initial_stack_pointer);
    set_gpr_lo(ctx, reg_a0, 0);
    set_gpr_lo(ctx, reg_a1, 0);
    set_gpr_lo(ctx, reg_ra, 0);
}

std::uint64_t gpr_lo(const R5900Context& ctx, unsigned reg) noexcept {
    std::uint64_t v[2];
    read_gpr(ctx, reg, v);
    return v[0];
}

std::uint64_t gpr_hi(const R5900Context& ctx, unsigned reg) noexcept {
    std::uint64_t v[2];
    read_gpr(ctx, reg, v);
    return v[1];
}

std::uint32_t gpr_u32(const R5900Context& ctx, unsigned reg) noexcept {
    return static_cast<std::uint32_t>(gpr_lo(ctx, reg));
}

void set_gpr_lo(R5900Context& ctx, unsigned reg, std::uint64_t value) noexcept {
    if ((reg & 31u) == 0) {
        return;
    }
    std::memcpy(&ctx.r[reg & 31u], &value, sizeof value);
}

std::string format_context(const R5900Context& ctx) {
    std::string out;
    char line[160];
    std::snprintf(line, sizeof line, "pc   %08X  hi %016llX  lo %016llX  sa %08X  status %08X\n",
                  static_cast<unsigned>(ctx.pc), static_cast<unsigned long long>(ctx.hi),
                  static_cast<unsigned long long>(ctx.lo), static_cast<unsigned>(ctx.sa),
                  static_cast<unsigned>(ctx.cop0_status));
    out += line;
    for (unsigned reg = 0; reg < 32; ++reg) {
        std::uint64_t v[2];
        read_gpr(ctx, reg, v);
        const std::string name(gpr_names[reg]);
        if (v[1] != 0) {
            std::snprintf(line, sizeof line, "%-4s %016llX_%016llX", name.c_str(),
                          static_cast<unsigned long long>(v[1]), static_cast<unsigned long long>(v[0]));
        } else {
            std::snprintf(line, sizeof line, "%-4s %016llX", name.c_str(),
                          static_cast<unsigned long long>(v[0]));
        }
        out += line;
        out += (reg % 4 == 3) ? "\n" : "  ";
    }
    return out;
}

} // namespace dq8::rt
