// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// TEMPORARY stand-in for include/dq8/abi/ps2recomp/ps2_runtime_macros.h, ps2_syscalls.h and ps2_stubs.h (M2
// session A), with only what the hand-written contract unit (contract_unit.cpp) uses. Same names and meaning
// as the recompiler's macros: GPR access with 32-bit results sign-extended into the low 64 bits, RAM fast
// paths masked into RDRAM, special addresses routed to the host, and by-name calls forwarded through
// call_named. When A's headers land, contract_unit.cpp includes them instead and this file is deleted.
#pragma once

#include "dq8/abi/ps2recomp/ps2_runtime.h"

#include <cstdint>
#include <cstring>

inline std::uint32_t dq8_shim_gpr_u32(const R5900Context* ctx, int reg) {
    if (reg == 0) {
        return 0;
    }
    std::uint32_t v = 0;
    std::memcpy(&v, &ctx->r[reg], sizeof v);
    return v;
}

inline void dq8_shim_set_gpr_s32(R5900Context* ctx, int reg, std::int32_t value) {
    if (reg == 0) {
        return;
    }
    const std::int64_t wide = value; // sign-extend into the low doubleword; the high doubleword is kept
    std::memcpy(&ctx->r[reg], &wide, sizeof wide);
}

inline std::uint32_t dq8_shim_read32(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime,
                                     std::uint32_t addr) {
    if (PS2Runtime::isSpecialAddress(addr)) {
        return runtime->Load32(rdram, ctx, addr);
    }
    std::uint32_t v = 0;
    std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), sizeof v);
    return v;
}

inline void dq8_shim_write32(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime, std::uint32_t addr,
                             std::uint32_t value) {
    if (PS2Runtime::isSpecialAddress(addr)) {
        runtime->Store32(rdram, ctx, addr, value);
        return;
    }
    std::memcpy(rdram + (addr & PS2_RAM_MASK), &value, sizeof value);
}

#define ADD32(a, b) static_cast<std::uint32_t>((a) + (b))
#define GPR_U32(ctx_ptr, reg_idx) dq8_shim_gpr_u32(ctx_ptr, reg_idx)
#define SET_GPR_U32(ctx_ptr, reg_idx, val)                                                                   \
    dq8_shim_set_gpr_s32(ctx_ptr, reg_idx, static_cast<std::int32_t>(val))
#define SET_GPR_S32(ctx_ptr, reg_idx, val)                                                                   \
    dq8_shim_set_gpr_s32(ctx_ptr, reg_idx, static_cast<std::int32_t>(val))
#define READ32(addr) dq8_shim_read32(rdram, ctx, runtime, static_cast<std::uint32_t>(addr))
#define WRITE32(addr, val)                                                                                   \
    dq8_shim_write32(rdram, ctx, runtime, static_cast<std::uint32_t>(addr), static_cast<std::uint32_t>(val))
// Our FAST_* route special addresses too (the emitter's constant-address hint path skips the test).
#define FAST_WRITE32(addr, val) WRITE32(addr, val)

namespace ps2_syscalls {
inline void FlushCache(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    runtime->api->call_named(runtime->host, DQ8_CALL_SYSCALL, "FlushCache", rdram, ctx);
}
} // namespace ps2_syscalls

namespace ps2_stubs {
inline void memcpy(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    runtime->api->call_named(runtime->host, DQ8_CALL_STUB, "memcpy", rdram, ctx);
}
} // namespace ps2_stubs
