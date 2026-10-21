// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xRuntime/src/lib/ps2_runtime.cpp (copyVu0ContextToState, copyVu0StateToContext).
#include "runtime/vu/vu0_bridge.h"

#include "dq8/abi/ps2recomp/r5900_context.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace dq8::rt {

namespace {

constexpr std::uint32_t vpu_stat_stopped_by_d = 1u << 1;
constexpr std::uint32_t vpu_stat_stopped_by_t = 1u << 2;
constexpr std::uint32_t fbrst_de0 = 1u << 2; // D bit enable, VU0
constexpr std::uint32_t fbrst_te0 = 1u << 3; // T bit enable, VU0
constexpr std::uint32_t one_float = 0x3F800000u;

void store4(float (&out)[4], __m128 v) {
    _mm_storeu_ps(out, v);
}

} // namespace

void load_vu0_state(const R5900Context& ctx, VuState& state) {
    const std::uint64_t cycles = state.cycles; // the interpreter's running count: callers measure deltas
    std::memset(&state, 0, sizeof(state));
    state.cycles = cycles;
    for (unsigned i = 0; i < 32; ++i) {
        store4(state.vf[i], ctx.vu0_vf[i]);
    }
    for (unsigned i = 0; i < 16; ++i) {
        state.vi[i] = static_cast<std::int16_t>(ctx.vi[i]);
    }
    store4(state.acc, ctx.vu0_acc);
    state.q = ctx.vu0_q;
    state.p = ctx.vu0_p;
    state.i = ctx.vu0_i;
    std::array<std::uint32_t, 4> r{};
    _mm_storeu_si128(reinterpret_cast<__m128i*>(r.data()), _mm_castps_si128(ctx.vu0_r));
    state.r = one_float | (r[0] & 0x007FFFFFu);
    state.mac = ctx.vu0_mac_flags;
    state.clip = ctx.vu0_clip_flags;
    state.status = ctx.vu0_status;
    state.itop = ctx.vu0_itop;
    state.dBitEnabled = (ctx.vu0_fbrst & fbrst_de0) != 0;
    state.tBitEnabled = (ctx.vu0_fbrst & fbrst_te0) != 0;
    state.vf[0][0] = 0.0f;
    state.vf[0][1] = 0.0f;
    state.vf[0][2] = 0.0f;
    state.vf[0][3] = 1.0f;
    state.vi[0] = 0;
}

void store_vu0_state(const VuState& state, R5900Context& ctx) {
    for (unsigned i = 0; i < 32; ++i) {
        ctx.vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
    }
    for (unsigned i = 0; i < 16; ++i) {
        ctx.vi[i] = static_cast<std::uint16_t>(state.vi[i]);
    }
    ctx.vu0_acc = _mm_loadu_ps(state.acc);
    ctx.vu0_q = state.q;
    ctx.vu0_p = state.p;
    ctx.vu0_i = state.i;
    ctx.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<std::int32_t>(state.r)));
    ctx.vu0_mac_flags = state.mac;
    ctx.vu0_clip_flags = state.clip;
    ctx.vu0_clip_flags2 = state.clip;
    ctx.vu0_status = static_cast<std::uint16_t>(state.status);
    ctx.vu0_itop = state.itop;
    ctx.vu0_pc = state.pc;
    ctx.vu0_tpc = state.pc;
    ctx.vu0_vpu_stat = (ctx.vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? vpu_stat_stopped_by_d : 0u) |
                       (state.stoppedByT ? vpu_stat_stopped_by_t : 0u);
    ctx.vu0_vpu_stat2 = 0;
    ctx.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    ctx.vi[0] = 0;
}

} // namespace dq8::rt
