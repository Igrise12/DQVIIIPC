// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// The EE register context and exception codes, field-for-field as PS2Recomp's ps2xRuntime/include/
// ps2_runtime.h at commit eb8688b2 (adapted: the debug dump and iostream dependency are removed). Generated
// code accesses fields by name, so names and types must match the recompiler's expectations; the module ABI
// version covers the layout (game_module.h).
#pragma once

#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#include <smmintrin.h>
#endif

enum PS2Exception {
    EXCEPTION_TLB_REFILL = 0x02,
    EXCEPTION_ADDRESS_ERROR_LOAD = 0x04,
    EXCEPTION_ADDRESS_ERROR_STORE = 0x05,
    EXCEPTION_SYSCALL = 0x08,
    EXCEPTION_BREAKPOINT = 0x09,
    EXCEPTION_RESERVED_INSTRUCTION = 0x0A,
    EXCEPTION_INTEGER_OVERFLOW = 0x0C,
    EXCEPTION_TRAP = 0x0D,
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // padded due to alignas(16): intended, the context holds SIMD registers
#endif
struct alignas(16) R5900Context {
    __m128i r[32]; // general-purpose registers, 128-bit

    std::uint32_t pc;
    std::uint64_t insn_count;
    std::uint64_t hi, lo;
    std::uint64_t hi1, lo1; // pipeline-1 HI/LO (MULT1, DIV1, MMI)
    std::uint32_t sa;

    // VU0 in macro mode
    __m128 vu0_vf[32];
    std::uint16_t vi[16];
    float vu0_q;
    float vu0_p;
    float vu0_i;
    __m128 vu0_r;
    __m128 vu0_acc;
    std::uint16_t vu0_status;
    std::uint32_t vu0_mac_flags;
    std::uint32_t vu0_clip_flags;
    std::uint32_t vu0_clip_flags2;
    std::uint32_t vu0_cmsar0;
    std::uint32_t vu0_cmsar1;
    std::uint32_t vu0_cmsar2;
    std::uint32_t vu0_cmsar3;
    std::uint32_t vu0_vpu_stat;
    std::uint32_t vu0_vpu_stat2;
    std::uint32_t vu0_vpu_stat3;
    std::uint32_t vu0_vpu_stat4;
    std::uint32_t vu0_tpc;
    std::uint32_t vu0_tpc2;
    std::uint32_t vu0_fbrst;
    std::uint32_t vu0_fbrst2;
    std::uint32_t vu0_fbrst3;
    std::uint32_t vu0_fbrst4;
    std::uint32_t vu0_itop;
    std::uint32_t vu0_top;
    std::uint32_t vu0_info;
    std::uint32_t vu0_xitop;
    std::uint32_t vu0_pc;

    float vu0_cf[4];

    // COP0
    std::uint32_t cop0_index;
    std::uint32_t cop0_random;
    std::uint32_t cop0_entrylo0;
    std::uint32_t cop0_entrylo1;
    std::uint32_t cop0_context;
    std::uint32_t cop0_pagemask;
    std::uint32_t cop0_wired;
    std::uint32_t cop0_badvaddr;
    std::uint32_t cop0_count;
    std::uint32_t cop0_entryhi;
    std::uint32_t cop0_compare;
    std::uint32_t cop0_status;
    std::uint32_t cop0_cause;
    std::uint32_t cop0_epc;
    std::uint32_t cop0_prid;
    std::uint32_t cop0_config;
    std::uint32_t cop0_badpaddr;
    std::uint32_t cop0_debug;
    std::uint32_t cop0_perf;
    std::uint32_t cop0_taglo;
    std::uint32_t cop0_taghi;
    std::uint32_t cop0_errorepc;

    // LL/SC reservation
    std::uint32_t llbit;
    std::uint32_t lladdr;

    // Delay-slot tracking
    bool in_delay_slot;
    std::uint32_t branch_pc;

    std::uint32_t cop2_ccr[32];

    // COP1
    float f[32];
    float f_acc;
    std::uint32_t fcr31;

    R5900Context() {
        std::memset(static_cast<void*>(this), 0, sizeof(*this));
        vu0_q = 1.0f;
        cop0_random = 47;
        cop0_status = 0x00010001u; // EIE | IE, as the EE kernel leaves it at hand-off
        cop0_prid = 0x00002e20u;   // R5900
        in_delay_slot = false;
        branch_pc = 0;
    }
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

inline std::uint32_t getRegU32(const R5900Context* ctx, int reg) {
    if (reg <= 0 || reg > 31) {
        return 0;
    }
    return static_cast<std::uint32_t>(_mm_cvtsi128_si32(ctx->r[reg]));
}
