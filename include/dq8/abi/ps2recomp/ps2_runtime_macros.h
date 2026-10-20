// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// The macros and helpers that PS2Recomp-generated code expands (it #includes "ps2_runtime_macros.h"). Adapted
// from ps2xRuntime/include/ps2_runtime_macros.h at commit eb8688b2. Every macro and helper the emitter at
// that commit writes is here, with upstream's results. What changed:
//   - It includes our ps2_runtime.h (the PS2Runtime adapter), not upstream's runtime.
//   - FAST_READn/FAST_WRITEn test PS2Runtime::isSpecialAddress like READn/WRITEn do. The emitter writes
//   FAST_*
//     for a constant address that upstream's predicate calls plain RAM, and that predicate misses VU0 code
//     and VU1 data (ps2_address.h has the whole map). With a literal address the test folds away.
//   - READn/WRITEn and the GPR accessors are inline functions behind the same macro names instead of lambdas
//   and
//     statement macros. Each argument is still evaluated once, and no macro declares a local that an argument
//     could shadow.
//   - Casts are static_casts or the ps2_as_* overloads (upstream's GCC vector casts do not build with MSVC);
//     ADD32_OV/SUB32_OV add in unsigned arithmetic (upstream's signed addition overflows, which is
//     undefined); the lane extractors use SSE2 only, so the GPR macros build without -msse4.1. The header is
//     clean under the project's warning set (cmake/CompilerWarnings.cmake), so host tests can use it.
// The MMI, VU and FPU helpers keep upstream's semantics, including where those differ from the hardware.
#pragma once

#include "ps2_runtime.h"

#include <algorithm> // std::max and std::min, which generated code calls
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <math.h> // sqrtf, which generated code calls unqualified

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#endif

// ---------------------------------------------------------------------------------------------------------------
// Lanes and bit helpers
// ---------------------------------------------------------------------------------------------------------------

inline std::int32_t Ps2ExtractEpi32(__m128i v, int index) noexcept {
    switch (index & 3) {
    case 0:
        return _mm_cvtsi128_si32(v);
    case 1:
        return _mm_cvtsi128_si32(_mm_shuffle_epi32(v, _MM_SHUFFLE(1, 1, 1, 1)));
    case 2:
        return _mm_cvtsi128_si32(_mm_shuffle_epi32(v, _MM_SHUFFLE(2, 2, 2, 2)));
    default:
        return _mm_cvtsi128_si32(_mm_shuffle_epi32(v, _MM_SHUFFLE(3, 3, 3, 3)));
    }
}

inline std::int64_t Ps2ExtractEpi64(__m128i v, int index) noexcept {
    return static_cast<std::int64_t>(_mm_cvtsi128_si64((index & 1) == 0 ? v : _mm_unpackhi_epi64(v, v)));
}

inline std::uint32_t ps2_clz32(std::uint32_t x) noexcept {
    return static_cast<std::uint32_t>(std::countl_zero(x));
}

inline std::uint64_t Ps2HiLoToU64(std::uint64_t hi, std::uint64_t lo) noexcept {
    return ((hi & 0xFFFFFFFFull) << 32) | (lo & 0xFFFFFFFFull);
}

inline std::uint64_t Ps2SignExt32ToU64(std::uint32_t v) noexcept {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(v)));
}

// PLZCW: the number of leading bits equal to the sign bit, minus one. 0x00000000 and 0xFFFFFFFF give 31.
inline std::uint32_t ps2_plzcw32(std::uint32_t x) noexcept {
    if (x == 0u || x == 0xFFFFFFFFu) {
        return 31u;
    }
    if ((x & 0x80000000u) != 0u) {
        x = ~x; // count leading ones as leading zeros
    }
    return static_cast<std::uint32_t>(std::countl_zero(x)) - 1u;
}

// Upstream casts macro arguments with GCC vector casts, (__m128i)(x) and (__m128)(x): the identity for the
// type the emitter passes and a bit reinterpretation for the other one. These overloads do the same
// everywhere.
inline __m128i ps2_as_epi(__m128i v) noexcept {
    return v;
}
inline __m128i ps2_as_epi(__m128 v) noexcept {
    return _mm_castps_si128(v);
}
inline __m128 ps2_as_ps(__m128 v) noexcept {
    return v;
}
inline __m128 ps2_as_ps(__m128i v) noexcept {
    return _mm_castsi128_ps(v);
}

#define PS2_BLENDV_PS(a, b, mask) _mm_blendv_ps((a), (b), (mask))
#define PS2_MIN_EPI32(a, b) _mm_min_epi32((a), (b))
#define PS2_MAX_EPI32(a, b) _mm_max_epi32((a), (b))
#define PS2_SHUFFLE_EPI8(v, mask) _mm_shuffle_epi8((v), (mask))

#define PS2_EXTRACT_EPI32(v, i) Ps2ExtractEpi32((v), (i))
#define PS2_EXTRACT_EPI64(v, i) Ps2ExtractEpi64((v), (i))

#define PS2_EXTRACT_EPI32_0(v) Ps2ExtractEpi32((v), 0)
#define PS2_EXTRACT_EPI32_1(v) Ps2ExtractEpi32((v), 1)
#define PS2_EXTRACT_EPI32_2(v) Ps2ExtractEpi32((v), 2)
#define PS2_EXTRACT_EPI32_3(v) Ps2ExtractEpi32((v), 3)

#define PS2_EXTRACT_EPI64_0(v) Ps2ExtractEpi64((v), 0)
#define PS2_EXTRACT_EPI64_1(v) Ps2ExtractEpi64((v), 1)

// ---------------------------------------------------------------------------------------------------------------
// 32-bit integer operations
// ---------------------------------------------------------------------------------------------------------------

#define ADD32(a, b) static_cast<std::uint32_t>((a) + (b))
#define ADD32_OV(rs, rt, result32, overflow)                                                                 \
    do {                                                                                                     \
        const std::int32_t ps2_ov_a_ = static_cast<std::int32_t>(rs);                                        \
        const std::int32_t ps2_ov_b_ = static_cast<std::int32_t>(rt);                                        \
        const std::int32_t ps2_ov_r_ = static_cast<std::int32_t>(static_cast<std::uint32_t>(ps2_ov_a_) +     \
                                                                 static_cast<std::uint32_t>(ps2_ov_b_));     \
        overflow = ((ps2_ov_a_ ^ ps2_ov_b_) >= 0) && ((ps2_ov_a_ ^ ps2_ov_r_) < 0);                          \
        result32 = static_cast<std::uint32_t>(ps2_ov_r_);                                                    \
    } while (0)
#define SUB32(a, b) static_cast<std::uint32_t>((a) - (b))
#define SUB32_OV(rs, rt, result32, overflow)                                                                 \
    do {                                                                                                     \
        const std::int32_t ps2_ov_a_ = static_cast<std::int32_t>(rs);                                        \
        const std::int32_t ps2_ov_b_ = static_cast<std::int32_t>(rt);                                        \
        const std::int32_t ps2_ov_r_ = static_cast<std::int32_t>(static_cast<std::uint32_t>(ps2_ov_a_) -     \
                                                                 static_cast<std::uint32_t>(ps2_ov_b_));     \
        overflow = ((ps2_ov_a_ ^ ps2_ov_b_) < 0) && ((ps2_ov_a_ ^ ps2_ov_r_) < 0);                           \
        result32 = static_cast<std::uint32_t>(ps2_ov_r_);                                                    \
    } while (0)
#define MUL32(a, b) static_cast<std::uint32_t>((a) * (b))
#define DIV32(a, b) static_cast<std::uint32_t>((a) / (b))
#define AND32(a, b) static_cast<std::uint32_t>((a) & (b))
#define OR32(a, b) static_cast<std::uint32_t>((a) | (b))
#define XOR32(a, b) static_cast<std::uint32_t>((a) ^ (b))
#define NOR32(a, b) static_cast<std::uint32_t>(~((a) | (b)))
#define SLL32(a, b) static_cast<std::uint32_t>((a) << (b))
#define SRL32(a, b) static_cast<std::uint32_t>((a) >> (b))
#define SRA32(a, b) static_cast<std::uint32_t>(static_cast<std::int32_t>(a) >> (b))
#define SLT32(a, b)                                                                                          \
    static_cast<std::uint32_t>(static_cast<std::int32_t>(a) < static_cast<std::int32_t>(b) ? 1 : 0)
#define SLTU32(a, b) static_cast<std::uint32_t>((a) < (b) ? 1 : 0)

// ---------------------------------------------------------------------------------------------------------------
// 128-bit MMI operations
// ---------------------------------------------------------------------------------------------------------------

#define PS2_PEXTLW(a, b) _mm_unpacklo_epi32(ps2_as_epi(b), ps2_as_epi(a))
#define PS2_PEXTUW(a, b) _mm_unpackhi_epi32(ps2_as_epi(b), ps2_as_epi(a))
#define PS2_PEXTLH(a, b) _mm_unpacklo_epi16(ps2_as_epi(b), ps2_as_epi(a))
#define PS2_PEXTUH(a, b) _mm_unpackhi_epi16(ps2_as_epi(b), ps2_as_epi(a))
#define PS2_PEXTLB(a, b) _mm_unpacklo_epi8(ps2_as_epi(b), ps2_as_epi(a))
#define PS2_PEXTUB(a, b) _mm_unpackhi_epi8(ps2_as_epi(b), ps2_as_epi(a))
#define PS2_PADDW(a, b) _mm_add_epi32(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PSUBW(a, b) _mm_sub_epi32(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PMAXW(a, b) PS2_MAX_EPI32(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PMINW(a, b) PS2_MIN_EPI32(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PADDH(a, b) _mm_add_epi16(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PSUBH(a, b) _mm_sub_epi16(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PMAXH(a, b) _mm_max_epi16(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PMINH(a, b) _mm_min_epi16(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PADDB(a, b) _mm_add_epi8(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PSUBB(a, b) _mm_sub_epi8(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PAND(a, b) _mm_and_si128(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_POR(a, b) _mm_or_si128(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PXOR(a, b) _mm_xor_si128(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PNOR(a, b) _mm_xor_si128(_mm_or_si128(ps2_as_epi(a), ps2_as_epi(b)), _mm_set1_epi32(-1))

// Packed compare greater than (PCGT) and equal (PCEQ)
#define PS2_PCGTW(a, b) _mm_cmpgt_epi32(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PCGTH(a, b) _mm_cmpgt_epi16(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PCGTB(a, b) _mm_cmpgt_epi8(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PCEQW(a, b) _mm_cmpeq_epi32(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PCEQH(a, b) _mm_cmpeq_epi16(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PCEQB(a, b) _mm_cmpeq_epi8(ps2_as_epi(a), ps2_as_epi(b))

// Packed absolute value (PABS)
#define PS2_PABSW(a) _mm_abs_epi32(ps2_as_epi(a))
#define PS2_PABSH(a) _mm_abs_epi16(ps2_as_epi(a))
#define PS2_PABSB(a) _mm_abs_epi8(ps2_as_epi(a))

// Packed add/subtract with signed saturation, word (PADDSW, PSUBSW)
inline __m128i ps2_paddsw(__m128i a, __m128i b) noexcept {
    const __m128i sum = _mm_add_epi32(a, b);
    // Overflow when both inputs differ in sign from the sum; saturate towards the sign of a.
    const __m128i overflow = _mm_srai_epi32(_mm_and_si128(_mm_xor_si128(a, sum), _mm_xor_si128(b, sum)), 31);
    const __m128i input_sign = _mm_srai_epi32(a, 31);
#if defined(__SSE4_1__)
    const __m128i sat = _mm_blendv_epi8(_mm_set1_epi32(INT32_MAX), _mm_set1_epi32(INT32_MIN), input_sign);
    return _mm_blendv_epi8(sum, sat, overflow);
#else
    const __m128i sat = _mm_or_si128(_mm_and_si128(input_sign, _mm_set1_epi32(INT32_MIN)),
                                     _mm_andnot_si128(input_sign, _mm_set1_epi32(INT32_MAX)));
    return _mm_or_si128(_mm_and_si128(overflow, sat), _mm_andnot_si128(overflow, sum));
#endif
}
#define PS2_PADDSW(a, b) ps2_paddsw(ps2_as_epi(a), ps2_as_epi(b))

inline __m128i ps2_psubsw(__m128i a, __m128i b) noexcept {
    const __m128i diff = _mm_sub_epi32(a, b);
    // Overflow when the inputs differ in sign and the difference differs in sign from a.
    const __m128i overflow = _mm_srai_epi32(_mm_and_si128(_mm_xor_si128(a, b), _mm_xor_si128(a, diff)), 31);
    const __m128i input_sign = _mm_srai_epi32(a, 31);
#if defined(__SSE4_1__)
    const __m128i sat = _mm_blendv_epi8(_mm_set1_epi32(INT32_MAX), _mm_set1_epi32(INT32_MIN), input_sign);
    return _mm_blendv_epi8(diff, sat, overflow);
#else
    const __m128i sat = _mm_or_si128(_mm_and_si128(input_sign, _mm_set1_epi32(INT32_MIN)),
                                     _mm_andnot_si128(input_sign, _mm_set1_epi32(INT32_MAX)));
    return _mm_or_si128(_mm_and_si128(overflow, sat), _mm_andnot_si128(overflow, diff));
#endif
}
#define PS2_PSUBSW(a, b) ps2_psubsw(ps2_as_epi(a), ps2_as_epi(b))

// Unsigned saturating word add and subtract (overflowing lanes become all ones, underflowing lanes zero)
inline __m128i ps2_paddu32(__m128i a, __m128i b) noexcept {
    const __m128i sum = _mm_add_epi32(a, b);
    const __m128i overflow = _mm_cmpgt_epi32(_mm_xor_si128(a, _mm_set1_epi32(INT32_MIN)),
                                             _mm_xor_si128(sum, _mm_set1_epi32(INT32_MIN)));
    return _mm_or_si128(sum, overflow);
}
inline __m128i ps2_psubu32(__m128i a, __m128i b) noexcept {
    const __m128i diff = _mm_sub_epi32(a, b);
    const __m128i underflow = _mm_cmpgt_epi32(_mm_xor_si128(b, _mm_set1_epi32(INT32_MIN)),
                                              _mm_xor_si128(a, _mm_set1_epi32(INT32_MIN)));
    return _mm_andnot_si128(underflow, diff);
}

// Packed pack (PPAC): the even elements of rt in the low half, those of rs in the high half
inline __m128i ps2_ppacw(__m128i rs, __m128i rt) noexcept {
    return _mm_castps_si128(
        _mm_shuffle_ps(_mm_castsi128_ps(rt), _mm_castsi128_ps(rs), _MM_SHUFFLE(2, 0, 2, 0)));
}
#define PS2_PPACW(a, b) ps2_ppacw(ps2_as_epi(a), ps2_as_epi(b))

inline __m128i ps2_ppach(__m128i rs, __m128i rt) noexcept {
    const __m128i mask = _mm_setr_epi8(0, 1, 4, 5, 8, 9, 12, 13, 0, 1, 4, 5, 8, 9, 12, 13);
    return _mm_unpacklo_epi64(_mm_shuffle_epi8(rt, mask), _mm_shuffle_epi8(rs, mask));
}
#define PS2_PPACH(a, b) ps2_ppach(ps2_as_epi(a), ps2_as_epi(b))

inline __m128i ps2_ppacb(__m128i rs, __m128i rt) noexcept {
    const __m128i mask = _mm_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, 0, 2, 4, 6, 8, 10, 12, 14);
    return _mm_unpacklo_epi64(_mm_shuffle_epi8(rt, mask), _mm_shuffle_epi8(rs, mask));
}
#define PS2_PPACB(a, b) ps2_ppacb(ps2_as_epi(a), ps2_as_epi(b))

// Packed interleave (PINT)
#define PS2_PINTH(a, b)                                                                                      \
    _mm_unpacklo_epi16(_mm_shuffle_epi32(ps2_as_epi(b), _MM_SHUFFLE(3, 2, 1, 0)),                            \
                       _mm_shuffle_epi32(ps2_as_epi(a), _MM_SHUFFLE(3, 2, 1, 0)))
#define PS2_PINTEH(a, b)                                                                                     \
    _mm_unpackhi_epi16(_mm_shuffle_epi32(ps2_as_epi(b), _MM_SHUFFLE(3, 2, 1, 0)),                            \
                       _mm_shuffle_epi32(ps2_as_epi(a), _MM_SHUFFLE(3, 2, 1, 0)))

// Packed multiply-add (PMADD)
#define PS2_PMADDW(a, b)                                                                                     \
    _mm_add_epi32(_mm_mullo_epi32(_mm_shuffle_epi32(ps2_as_epi(a), _MM_SHUFFLE(1, 0, 3, 2)),                 \
                                  _mm_shuffle_epi32(ps2_as_epi(b), _MM_SHUFFLE(1, 0, 3, 2))),                \
                  _mm_mullo_epi32(_mm_shuffle_epi32(ps2_as_epi(a), _MM_SHUFFLE(3, 2, 1, 0)),                 \
                                  _mm_shuffle_epi32(ps2_as_epi(b), _MM_SHUFFLE(3, 2, 1, 0))))

// Packed variable shifts, lane by lane on the four words (shift count: the low five bits of each word of b)
inline __m128i ps2_sllv_epi32(__m128i a, __m128i count) noexcept {
    std::uint32_t va[4];
    std::uint32_t vc[4];
    std::memcpy(va, &a, sizeof(va));
    std::memcpy(vc, &count, sizeof(vc));
    for (int i = 0; i < 4; ++i) {
        va[i] <<= (vc[i] & 0x1Fu);
    }
    __m128i out;
    std::memcpy(&out, va, sizeof(out));
    return out;
}
inline __m128i ps2_srlv_epi32(__m128i a, __m128i count) noexcept {
    std::uint32_t va[4];
    std::uint32_t vc[4];
    std::memcpy(va, &a, sizeof(va));
    std::memcpy(vc, &count, sizeof(vc));
    for (int i = 0; i < 4; ++i) {
        va[i] >>= (vc[i] & 0x1Fu);
    }
    __m128i out;
    std::memcpy(&out, va, sizeof(out));
    return out;
}
inline __m128i ps2_srav_epi32(__m128i a, __m128i count) noexcept {
    std::int32_t va[4];
    std::uint32_t vc[4];
    std::memcpy(va, &a, sizeof(va));
    std::memcpy(vc, &count, sizeof(vc));
    for (int i = 0; i < 4; ++i) {
        va[i] >>= (vc[i] & 0x1Fu);
    }
    __m128i out;
    std::memcpy(&out, va, sizeof(out));
    return out;
}
#define PS2_PSLLVW(a, b) ps2_sllv_epi32(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PSRLVW(a, b) ps2_srlv_epi32(ps2_as_epi(a), ps2_as_epi(b))
#define PS2_PSRAVW(a, b) ps2_srav_epi32(ps2_as_epi(a), ps2_as_epi(b))

// PMFHL: combine HI and LO
inline __m128i ps2_u64_to_epi64_pair(std::uint64_t value) noexcept {
    return _mm_set1_epi64x(static_cast<long long>(value));
}

#define PS2_PMFHL_LW(hi, lo) _mm_unpacklo_epi64(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_UW(hi, lo) _mm_unpackhi_epi64(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_SLW(hi, lo) _mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_LH(hi, lo)                                                                                 \
    _mm_shuffle_epi32(_mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi)),                 \
                      _MM_SHUFFLE(3, 1, 2, 0))
#define PS2_PMFHL_SH(hi, lo)                                                                                 \
    _mm_shufflehi_epi16(                                                                                     \
        _mm_shufflelo_epi16(_mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi)),           \
                            _MM_SHUFFLE(3, 1, 2, 0)),                                                        \
        _MM_SHUFFLE(3, 1, 2, 0))

// QFSRV: (rs || rt) as 256 bits, shifted right by SA bits, low 128 bits
inline __m128i ps2_qfsrv(__m128i rs, __m128i rt, std::uint32_t sa) noexcept {
    if (sa == 0u) {
        return rt;
    }
    if (sa >= 128u) {
        if (sa >= 256u) {
            return _mm_setzero_si128();
        }
        const std::uint32_t shift = sa - 128u;
        if (shift == 0u) {
            return rs;
        }
        const std::uint32_t byte_shift = shift / 8u;
        const int bit_shift = static_cast<int>(shift % 8u);
        std::uint8_t src[16];
        std::uint8_t buf[16] = {};
        std::memcpy(src, &rs, sizeof(src));
        for (std::uint32_t i = 0; i + byte_shift < 16u; ++i) {
            buf[i] = src[i + byte_shift];
        }
        __m128i result;
        std::memcpy(&result, buf, sizeof(result));
        if (bit_shift > 0) {
            result = _mm_or_si128(_mm_srli_epi64(result, bit_shift),
                                  _mm_slli_epi64(_mm_bsrli_si128(result, 8), 64 - bit_shift));
        }
        return result;
    }
    // sa is 1..127
    const std::uint32_t byte_shift = sa / 8u;
    const std::uint32_t bit_shift = sa % 8u;
    std::uint8_t combined[32];
    std::memcpy(combined, &rt, 16);      // low 128 bits
    std::memcpy(combined + 16, &rs, 16); // high 128 bits
    if (bit_shift == 0u) {
        std::uint8_t shifted[16];
        for (std::uint32_t i = 0; i < 16u; ++i) {
            shifted[i] = (i + byte_shift < 32u) ? combined[i + byte_shift] : std::uint8_t{0};
        }
        __m128i result;
        std::memcpy(&result, shifted, sizeof(result));
        return result;
    }
    std::uint64_t lo0;
    std::uint64_t lo1;
    std::uint64_t hi0;
    std::uint64_t hi1;
    std::memcpy(&lo0, combined, 8);
    std::memcpy(&lo1, combined + 8, 8);
    std::memcpy(&hi0, combined + 16, 8);
    std::memcpy(&hi1, combined + 24, 8);
    std::uint64_t r0;
    std::uint64_t r1;
    if (sa < 64u) {
        r0 = (lo0 >> sa) | (lo1 << (64u - sa));
        r1 = (lo1 >> sa) | (hi0 << (64u - sa));
    } else {
        const std::uint32_t s = sa - 64u; // never 0 here: sa is not a multiple of 8
        r0 = (lo1 >> s) | (hi0 << (64u - s));
        r1 = (hi0 >> s) | (hi1 << (64u - s));
    }
    return _mm_set_epi64x(static_cast<long long>(r1), static_cast<long long>(r0));
}
#define PS2_QFSRV(rs, rt, sa) ps2_qfsrv(ps2_as_epi(rs), ps2_as_epi(rt), static_cast<std::uint32_t>(sa))
#define PS2_PCPYLD(rs, rt) _mm_unpacklo_epi64((rt), (rs))
#define PS2_PEXEH(rs)                                                                                        \
    _mm_shufflelo_epi16(_mm_shufflehi_epi16((rs), _MM_SHUFFLE(2, 3, 0, 1)), _MM_SHUFFLE(2, 3, 0, 1))
#define PS2_PEXEW(rs) _mm_shuffle_epi32((rs), _MM_SHUFFLE(2, 3, 0, 1))
#define PS2_PROT3W(rs) _mm_shuffle_epi32((rs), _MM_SHUFFLE(0, 3, 2, 1))

// ---------------------------------------------------------------------------------------------------------------
// PS2 float semantics (EE FPU and VU0 macro mode)
// ---------------------------------------------------------------------------------------------------------------
// The PS2 format has no Inf, NaN or denormals:
//   - exponent 255 is an ordinary exponent, so 0x7F800000 is 2^128 and the largest value is 0x7FFFFFFF;
//   - exponent 0 is zero whatever the mantissa holds;
//   - a result of 2^129 or more saturates to +-0x7FFFFFFF, one below 2^-126 becomes +-0;
//   - x/0 gives +-0x7FFFFFFF.
// A host float cannot hold the exponent-255 values, so anything that touches them is computed in double. That
// path is rare and kept out of line; the inline part is the host operation and a range check. One case is
// left to the host: ADD.S/SUB.S/MUL.S and the vector operations do not look for an operand with exponent 0
// and a non-zero mantissa, so it takes part with its tiny host value instead of as zero. Upstream checked
// this against the hardware results in ps2autotests (tests/cpu/ee_fpu).
#if defined(_MSC_VER)
#define PS2_FLOAT_NOINLINE __declspec(noinline)
#else
#define PS2_FLOAT_NOINLINE __attribute__((noinline))
#endif

inline constexpr std::uint32_t PS2_FLOAT_MAX_BITS = 0x7FFFFFFFu;
inline constexpr std::uint32_t PS2_FLOAT_EXP_MASK = 0x7F800000u;
inline constexpr std::uint32_t PS2_FLOAT_SIGN_MASK = 0x80000000u;

// True when a host float operation cannot be trusted with x: exponent 255, or exponent 0 with a mantissa.
inline bool ps2_fspecial(float x) noexcept {
    const std::uint32_t t = std::bit_cast<std::uint32_t>(x) & 0x7FFFFFFFu;
    return t != 0u && (t - 0x00800000u) >= 0x7F000000u;
}

inline double ps2_to_double(float x) noexcept {
    const std::uint32_t b = std::bit_cast<std::uint32_t>(x);
    const std::uint32_t exponent = b & PS2_FLOAT_EXP_MASK;
    if (exponent == 0u) {
        return static_cast<double>(std::bit_cast<float>(b & PS2_FLOAT_SIGN_MASK));
    }
    if (exponent == PS2_FLOAT_EXP_MASK) {
        return static_cast<double>(std::bit_cast<float>(b - 0x00800000u)) * 2.0;
    }
    return static_cast<double>(x);
}

inline float ps2_from_double(double d) noexcept {
    const double magnitude = std::fabs(d);
    const std::uint32_t sign = std::signbit(d) ? PS2_FLOAT_SIGN_MASK : 0u;
    if (magnitude >= 0x1p129) {
        return std::bit_cast<float>(sign | PS2_FLOAT_MAX_BITS);
    }
    if (magnitude < 0x1p-126) {
        return std::bit_cast<float>(sign);
    }
    if (magnitude >= 0x1p128) {
        // Convert one exponent lower, then put the exponent back as 255.
        const std::uint32_t b = std::bit_cast<std::uint32_t>(static_cast<float>(d * 0.5));
        if ((b & PS2_FLOAT_EXP_MASK) == PS2_FLOAT_EXP_MASK) {
            return std::bit_cast<float>(sign | PS2_FLOAT_MAX_BITS);
        }
        return std::bit_cast<float>(b + 0x00800000u);
    }
    // A value that rounds up to 2^128 comes out as the host's Inf pattern, which is 2^128 on the PS2.
    return static_cast<float>(d);
}

// Zero operands and results end up in the *_slow functions too; the host result is right for them.
PS2_FLOAT_NOINLINE inline float ps2_fadd_slow(float a, float b) noexcept {
    const float r = a + b;
    if (!(ps2_fspecial(a) || ps2_fspecial(b) || ps2_fspecial(r))) {
        return r;
    }
    return ps2_from_double(ps2_to_double(a) + ps2_to_double(b));
}

PS2_FLOAT_NOINLINE inline float ps2_fsub_slow(float a, float b) noexcept {
    const float r = a - b;
    if (!(ps2_fspecial(a) || ps2_fspecial(b) || ps2_fspecial(r))) {
        return r;
    }
    return ps2_from_double(ps2_to_double(a) - ps2_to_double(b));
}

PS2_FLOAT_NOINLINE inline float ps2_fmul_slow(float a, float b) noexcept {
    const float r = a * b;
    if (!(ps2_fspecial(a) || ps2_fspecial(b) || ps2_fspecial(r))) {
        return r;
    }
    return ps2_from_double(ps2_to_double(a) * ps2_to_double(b));
}

// A zero divisor gives +-max with the sign of fs xor ft.
PS2_FLOAT_NOINLINE inline float ps2_fdiv_slow(float a, float b) noexcept {
    const std::uint32_t ua = std::bit_cast<std::uint32_t>(a);
    const std::uint32_t ub = std::bit_cast<std::uint32_t>(b);
    if ((ub & PS2_FLOAT_EXP_MASK) == 0u) {
        return std::bit_cast<float>(((ua ^ ub) & PS2_FLOAT_SIGN_MASK) | PS2_FLOAT_MAX_BITS);
    }
    const float r = a / b;
    if (!(ps2_fspecial(a) || ps2_fspecial(b) || ps2_fspecial(r))) {
        return r;
    }
    return ps2_from_double(ps2_to_double(a) / ps2_to_double(b));
}

// sqrt(|ft|); the result is never negative, sqrt(-0) is +0.
PS2_FLOAT_NOINLINE inline float ps2_fsqrt_slow(float a) noexcept {
    if ((std::bit_cast<std::uint32_t>(a) & PS2_FLOAT_EXP_MASK) == 0u) {
        return 0.0f;
    }
    return ps2_from_double(std::sqrt(std::fabs(ps2_to_double(a))));
}

// fs / sqrt(|ft|). The sign of ft is ignored everywhere, so a zero ft gives +-max with the sign of fs alone.
PS2_FLOAT_NOINLINE inline float ps2_frsqrt_slow(float a, float b) noexcept {
    if ((std::bit_cast<std::uint32_t>(b) & PS2_FLOAT_EXP_MASK) == 0u) {
        return std::bit_cast<float>((std::bit_cast<std::uint32_t>(a) & PS2_FLOAT_SIGN_MASK) |
                                    PS2_FLOAT_MAX_BITS);
    }
    // The root is rounded to a float before the division, as on the hardware.
    const float root = ps2_from_double(std::sqrt(std::fabs(ps2_to_double(b))));
    return ps2_from_double(ps2_to_double(a) / static_cast<double>(root));
}

// True when a, b and the host result r are all normal numbers, so r is also the PS2 result.
inline bool ps2_fall_normal(float a, float b, float r) noexcept {
    // Doubling drops the sign; adding one exponent step wraps exponent 255 round to 0 next to exponent 0.
    const std::uint32_t sa = std::bit_cast<std::uint32_t>(a) * 2u + 0x01000000u;
    const std::uint32_t sb = std::bit_cast<std::uint32_t>(b) * 2u + 0x01000000u;
    const std::uint32_t sr = std::bit_cast<std::uint32_t>(r) * 2u + 0x01000000u;
    return std::min(std::min(sa, sb), sr) >= 0x02000000u;
}

// The same test on the result alone. An exponent-255 operand always leaves exponent 255 in the host result of
// an addition, subtraction or multiplication (Inf or NaN), so this finds those operands as well.
inline bool ps2_fnormal(float r) noexcept {
    return (std::bit_cast<std::uint32_t>(r) * 2u + 0x01000000u) >= 0x02000000u;
}

inline float ps2_fadd(float a, float b) noexcept {
    const float r = a + b;
    return ps2_fnormal(r) ? r : ps2_fadd_slow(a, b);
}

inline float ps2_fsub(float a, float b) noexcept {
    const float r = a - b;
    return ps2_fnormal(r) ? r : ps2_fsub_slow(a, b);
}

inline float ps2_fmul(float a, float b) noexcept {
    const float r = a * b;
    return ps2_fnormal(r) ? r : ps2_fmul_slow(a, b);
}

inline float ps2_fdiv(float a, float b) noexcept {
    const float r = a / b;
    return ps2_fall_normal(a, b, r) ? r : ps2_fdiv_slow(a, b);
}

inline float ps2_fsqrt(float a) noexcept {
    const float r = std::sqrt(a);
    return ps2_fall_normal(a, a, r) ? r : ps2_fsqrt_slow(a);
}

inline float ps2_frsqrt(float a, float b) noexcept {
    const float r = a / std::sqrt(b);
    return ps2_fall_normal(a, b, r) ? r : ps2_frsqrt_slow(a, b);
}

enum class Ps2VecOp { Add, Sub, Mul };

// Lane by lane through the scalar helpers; only reached when a result lane has exponent 255.
PS2_FLOAT_NOINLINE inline __m128 ps2_vslow(__m128 a, __m128 b, Ps2VecOp op) noexcept {
    float la[4];
    float lb[4];
    float lr[4];
    std::memcpy(la, &a, sizeof(la));
    std::memcpy(lb, &b, sizeof(lb));
    for (int i = 0; i < 4; ++i) {
        switch (op) {
        case Ps2VecOp::Add:
            lr[i] = ps2_fadd_slow(la[i], lb[i]);
            break;
        case Ps2VecOp::Sub:
            lr[i] = ps2_fsub_slow(la[i], lb[i]);
            break;
        default:
            lr[i] = ps2_fmul_slow(la[i], lb[i]);
            break;
        }
    }
    __m128 out;
    std::memcpy(&out, lr, sizeof(out));
    return out;
}

// r is the host result of a op b. As in ps2_fnormal, testing r alone finds every lane that needs the double
// path. Results with exponent 0 are flushed to +-0.
inline __m128 ps2_vfinish(__m128 a, __m128 b, __m128 r, Ps2VecOp op) noexcept {
    const __m128i exp_mask = _mm_set1_epi32(0x7F800000);
    const __m128i rexp = _mm_and_si128(_mm_castps_si128(r), exp_mask);
    if (_mm_movemask_epi8(_mm_cmpeq_epi32(rexp, exp_mask)) != 0) {
        return ps2_vslow(a, b, op);
    }
    const __m128i flush =
        _mm_and_si128(_mm_cmpeq_epi32(rexp, _mm_setzero_si128()), _mm_set1_epi32(0x7FFFFFFF));
    return _mm_castsi128_ps(_mm_andnot_si128(flush, _mm_castps_si128(r)));
}

inline __m128 ps2_vadd(__m128 a, __m128 b) noexcept {
    return ps2_vfinish(a, b, _mm_add_ps(a, b), Ps2VecOp::Add);
}

inline __m128 ps2_vsub(__m128 a, __m128 b) noexcept {
    return ps2_vfinish(a, b, _mm_sub_ps(a, b), Ps2VecOp::Sub);
}

inline __m128 ps2_vmul(__m128 a, __m128 b) noexcept {
    return ps2_vfinish(a, b, _mm_mul_ps(a, b), Ps2VecOp::Mul);
}

// VU0 macro-mode operations
#define PS2_VADD(a, b) ps2_vadd(ps2_as_ps(a), ps2_as_ps(b))
#define PS2_VSUB(a, b) ps2_vsub(ps2_as_ps(a), ps2_as_ps(b))
#define PS2_VMUL(a, b) ps2_vmul(ps2_as_ps(a), ps2_as_ps(b))
#define PS2_VDIV(a, b) _mm_div_ps(ps2_as_ps(a), ps2_as_ps(b))
#define PS2_VMULQ(a, q) _mm_mul_ps(ps2_as_ps(a), _mm_set1_ps(q))
#define PS2_VBLEND(a, b, mask) PS2_BLENDV_PS(ps2_as_ps(a), ps2_as_ps(b), ps2_as_ps(mask))
#define PS2_VSQRT(x) std::sqrt(static_cast<float>(x))
#define PS2_VRSQRT(x) (1.0f / std::sqrt(static_cast<float>(x)))

// FPU (COP1) operations
#define FPU_SET_ACC(ctx_ptr, res) ((ctx_ptr)->f_acc = (res))
#define FPU_ADD_S(a, b) ps2_fadd(static_cast<float>(a), static_cast<float>(b))
#define FPU_SUB_S(a, b) ps2_fsub(static_cast<float>(a), static_cast<float>(b))
#define FPU_MUL_S(a, b) ps2_fmul(static_cast<float>(a), static_cast<float>(b))
#define FPU_DIV_S(a, b) ps2_fdiv(static_cast<float>(a), static_cast<float>(b))
#define FPU_RSQRT_S(a, b) ps2_frsqrt(static_cast<float>(a), static_cast<float>(b))
#define FPU_SQRT_S(a) ps2_fsqrt(static_cast<float>(a))
#define FPU_ABS_S(a) std::fabs(static_cast<float>(a))
#define FPU_MOV_S(a) static_cast<float>(a)
#define FPU_NEG_S(a) (-static_cast<float>(a))
#define FPU_ROUND_L_S(a) static_cast<std::int64_t>(std::round(static_cast<float>(a)))
#define FPU_TRUNC_L_S(a) static_cast<std::int64_t>(static_cast<float>(a))
#define FPU_CEIL_L_S(a) static_cast<std::int64_t>(std::ceil(static_cast<float>(a)))
#define FPU_FLOOR_L_S(a) static_cast<std::int64_t>(std::floor(static_cast<float>(a)))
#define FPU_ROUND_W_S(a) static_cast<std::int32_t>(std::nearbyint(static_cast<float>(a)))
#define FPU_TRUNC_W_S(a) static_cast<std::int32_t>(static_cast<float>(a))
#define FPU_CEIL_W_S(a) static_cast<std::int32_t>(std::ceil(static_cast<float>(a)))
#define FPU_FLOOR_W_S(a) static_cast<std::int32_t>(std::floor(static_cast<float>(a)))
#define FPU_CVT_S_W(a) static_cast<float>(static_cast<std::int32_t>(a))
#define FPU_CVT_S_L(a) static_cast<float>(static_cast<std::int64_t>(a))
#define FPU_CVT_W_S(a) static_cast<std::int32_t>(std::nearbyint(static_cast<float>(a)))
#define FPU_CVT_L_S(a) static_cast<std::int64_t>(static_cast<float>(a))
#define PS2_FPU_UNORDERED(a, b) (std::isnan(static_cast<float>(a)) || std::isnan(static_cast<float>(b)))
#define FPU_C_F_S(a, b) false
#define FPU_C_UN_S(a, b) PS2_FPU_UNORDERED(a, b)
#define FPU_C_EQ_S(a, b) (static_cast<float>(a) == static_cast<float>(b))
#define FPU_C_UEQ_S(a, b) (static_cast<float>(a) == static_cast<float>(b) || PS2_FPU_UNORDERED(a, b))
#define FPU_C_OLT_S(a, b) (static_cast<float>(a) < static_cast<float>(b))
#define FPU_C_ULT_S(a, b) (static_cast<float>(a) < static_cast<float>(b) || PS2_FPU_UNORDERED(a, b))
#define FPU_C_OLE_S(a, b) (static_cast<float>(a) <= static_cast<float>(b))
#define FPU_C_ULE_S(a, b) (static_cast<float>(a) <= static_cast<float>(b) || PS2_FPU_UNORDERED(a, b))
#define FPU_C_SF_S(a, b) false
#define FPU_C_NGLE_S(a, b) PS2_FPU_UNORDERED(a, b)
#define FPU_C_SEQ_S(a, b) (static_cast<float>(a) == static_cast<float>(b))
#define FPU_C_NGL_S(a, b) (static_cast<float>(a) == static_cast<float>(b) || PS2_FPU_UNORDERED(a, b))
#define FPU_C_LT_S(a, b) (static_cast<float>(a) < static_cast<float>(b))
#define FPU_C_NGE_S(a, b) (static_cast<float>(a) < static_cast<float>(b) || PS2_FPU_UNORDERED(a, b))
#define FPU_C_LE_S(a, b) (static_cast<float>(a) <= static_cast<float>(b))
#define FPU_C_NGT_S(a, b) (static_cast<float>(a) <= static_cast<float>(b) || PS2_FPU_UNORDERED(a, b))

// ---------------------------------------------------------------------------------------------------------------
// Memory access
// ---------------------------------------------------------------------------------------------------------------
// RAM (everything ps2_is_special_address rejects) is read and written in place, masked to 32 MiB; an access
// that runs past the end of RAM wraps byte by byte. Everything else goes to the host's Load/Store.

inline bool Ps2FastRangeIsContiguous(std::uint32_t offset, std::uint32_t bytes) noexcept {
    return offset <= (PS2_RAM_SIZE - bytes);
}

template <typename T> inline T ps2_fast_read(const std::uint8_t* rdram, std::uint32_t addr) noexcept {
    constexpr std::uint32_t size = sizeof(T);
    const std::uint32_t offset = addr & PS2_RAM_MASK;
    T value;
    if (Ps2FastRangeIsContiguous(offset, size)) {
        std::memcpy(&value, rdram + offset, size);
    } else {
        std::uint8_t wrapped[size];
        for (std::uint32_t i = 0; i < size; ++i) {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        std::memcpy(&value, wrapped, size);
    }
    return value;
}

template <typename T> inline void ps2_fast_write(std::uint8_t* rdram, std::uint32_t addr, T value) noexcept {
    constexpr std::uint32_t size = sizeof(T);
    const std::uint32_t offset = addr & PS2_RAM_MASK;
    if (Ps2FastRangeIsContiguous(offset, size)) {
        std::memcpy(rdram + offset, &value, size);
    } else {
        std::uint8_t wrapped[size];
        std::memcpy(wrapped, &value, size);
        for (std::uint32_t i = 0; i < size; ++i) {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
    }
}

inline std::uint8_t Ps2FastRead8(const std::uint8_t* rdram, std::uint32_t addr) noexcept {
    return rdram[addr & PS2_RAM_MASK];
}
inline std::uint16_t Ps2FastRead16(const std::uint8_t* rdram, std::uint32_t addr) noexcept {
    return ps2_fast_read<std::uint16_t>(rdram, addr);
}
inline std::uint32_t Ps2FastRead32(const std::uint8_t* rdram, std::uint32_t addr) noexcept {
    return ps2_fast_read<std::uint32_t>(rdram, addr);
}
inline std::uint64_t Ps2FastRead64(const std::uint8_t* rdram, std::uint32_t addr) noexcept {
    return ps2_fast_read<std::uint64_t>(rdram, addr);
}
inline __m128i Ps2FastRead128(const std::uint8_t* rdram, std::uint32_t addr) noexcept {
    return ps2_fast_read<__m128i>(rdram, addr);
}

inline void Ps2FastWrite8(std::uint8_t* rdram, std::uint32_t addr, std::uint8_t value) noexcept {
    rdram[addr & PS2_RAM_MASK] = value;
}
inline void Ps2FastWrite16(std::uint8_t* rdram, std::uint32_t addr, std::uint16_t value) noexcept {
    ps2_fast_write(rdram, addr, value);
}
inline void Ps2FastWrite32(std::uint8_t* rdram, std::uint32_t addr, std::uint32_t value) noexcept {
    ps2_fast_write(rdram, addr, value);
}
inline void Ps2FastWrite64(std::uint8_t* rdram, std::uint32_t addr, std::uint64_t value) noexcept {
    ps2_fast_write(rdram, addr, value);
}
inline void Ps2FastWrite128(std::uint8_t* rdram, std::uint32_t addr, __m128i value) noexcept {
    ps2_fast_write(rdram, addr, value);
}

// Routed loads: the host for special addresses, RAM otherwise. READn and FAST_READn both come here.
inline std::uint8_t Ps2Read8(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime,
                             std::uint32_t addr) {
    return PS2Runtime::isSpecialAddress(addr) ? runtime->Load8(rdram, ctx, addr) : Ps2FastRead8(rdram, addr);
}
inline std::uint16_t Ps2Read16(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime,
                               std::uint32_t addr) {
    return PS2Runtime::isSpecialAddress(addr) ? runtime->Load16(rdram, ctx, addr)
                                              : Ps2FastRead16(rdram, addr);
}
inline std::uint32_t Ps2Read32(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime,
                               std::uint32_t addr) {
    return PS2Runtime::isSpecialAddress(addr) ? runtime->Load32(rdram, ctx, addr)
                                              : Ps2FastRead32(rdram, addr);
}
inline std::uint64_t Ps2Read64(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime,
                               std::uint32_t addr) {
    return PS2Runtime::isSpecialAddress(addr) ? runtime->Load64(rdram, ctx, addr)
                                              : Ps2FastRead64(rdram, addr);
}
inline __m128i Ps2Read128(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime, std::uint32_t addr) {
    return PS2Runtime::isSpecialAddress(addr) ? runtime->Load128(rdram, ctx, addr)
                                              : Ps2FastRead128(rdram, addr);
}

// Routed stores. WRITEn traces the RAM path as upstream does; the emitter traces before FAST_WRITEn itself.
inline void Ps2Write8(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime, std::uint32_t addr,
                      std::uint8_t value, bool trace) {
    if (PS2Runtime::isSpecialAddress(addr)) {
        runtime->Store8(rdram, ctx, addr, value);
        return;
    }
    if (trace) {
        ps2TraceGuestWrite(rdram, addr, 1u, value, 0u, "WRITE8", ctx);
    }
    Ps2FastWrite8(rdram, addr, value);
}
inline void Ps2Write16(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime, std::uint32_t addr,
                       std::uint16_t value, bool trace) {
    if (PS2Runtime::isSpecialAddress(addr)) {
        runtime->Store16(rdram, ctx, addr, value);
        return;
    }
    if (trace) {
        ps2TraceGuestWrite(rdram, addr, 2u, value, 0u, "WRITE16", ctx);
    }
    Ps2FastWrite16(rdram, addr, value);
}
inline void Ps2Write32(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime, std::uint32_t addr,
                       std::uint32_t value, bool trace) {
    if (PS2Runtime::isSpecialAddress(addr)) {
        runtime->Store32(rdram, ctx, addr, value);
        return;
    }
    if (trace) {
        ps2TraceGuestWrite(rdram, addr, 4u, value, 0u, "WRITE32", ctx);
    }
    Ps2FastWrite32(rdram, addr, value);
}
inline void Ps2Write64(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime, std::uint32_t addr,
                       std::uint64_t value, bool trace) {
    if (PS2Runtime::isSpecialAddress(addr)) {
        runtime->Store64(rdram, ctx, addr, value);
        return;
    }
    if (trace) {
        ps2TraceGuestWrite(rdram, addr, 8u, value, 0u, "WRITE64", ctx);
    }
    Ps2FastWrite64(rdram, addr, value);
}
inline void Ps2Write128(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime, std::uint32_t addr,
                        __m128i value, bool trace) {
    if (PS2Runtime::isSpecialAddress(addr)) {
        runtime->Store128(rdram, ctx, addr, value);
        return;
    }
    if (trace) {
        const auto lo = static_cast<std::uint64_t>(PS2_EXTRACT_EPI64_0(value));
        const auto hi = static_cast<std::uint64_t>(PS2_EXTRACT_EPI64_1(value));
        ps2TraceGuestWrite(rdram, addr, 16u, lo, hi, "WRITE128", ctx);
    }
    Ps2FastWrite128(rdram, addr, value);
}

// These expand inside a generated function, where rdram, ctx and runtime are the function's parameters.
#define READ8(addr) Ps2Read8(rdram, ctx, runtime, static_cast<std::uint32_t>(addr))
#define READ16(addr) Ps2Read16(rdram, ctx, runtime, static_cast<std::uint32_t>(addr))
#define READ32(addr) Ps2Read32(rdram, ctx, runtime, static_cast<std::uint32_t>(addr))
#define READ64(addr) Ps2Read64(rdram, ctx, runtime, static_cast<std::uint32_t>(addr))
#define READ128(addr) Ps2Read128(rdram, ctx, runtime, static_cast<std::uint32_t>(addr))

#define WRITE8(addr, val)                                                                                    \
    Ps2Write8(rdram, ctx, runtime, static_cast<std::uint32_t>(addr), static_cast<std::uint8_t>(val), true)
#define WRITE16(addr, val)                                                                                   \
    Ps2Write16(rdram, ctx, runtime, static_cast<std::uint32_t>(addr), static_cast<std::uint16_t>(val), true)
#define WRITE32(addr, val)                                                                                   \
    Ps2Write32(rdram, ctx, runtime, static_cast<std::uint32_t>(addr), static_cast<std::uint32_t>(val), true)
#define WRITE64(addr, val)                                                                                   \
    Ps2Write64(rdram, ctx, runtime, static_cast<std::uint32_t>(addr), static_cast<std::uint64_t>(val), true)
#define WRITE128(addr, val) Ps2Write128(rdram, ctx, runtime, static_cast<std::uint32_t>(addr), (val), true)

// Upstream's FAST_* touch RAM without a test; ours are routed (see the top of this file).
#define FAST_READ8(addr) READ8(addr)
#define FAST_READ16(addr) READ16(addr)
#define FAST_READ32(addr) READ32(addr)
#define FAST_READ64(addr) READ64(addr)
#define FAST_READ128(addr) READ128(addr)

#define FAST_WRITE8(addr, val)                                                                               \
    Ps2Write8(rdram, ctx, runtime, static_cast<std::uint32_t>(addr), static_cast<std::uint8_t>(val), false)
#define FAST_WRITE16(addr, val)                                                                              \
    Ps2Write16(rdram, ctx, runtime, static_cast<std::uint32_t>(addr), static_cast<std::uint16_t>(val), false)
#define FAST_WRITE32(addr, val)                                                                              \
    Ps2Write32(rdram, ctx, runtime, static_cast<std::uint32_t>(addr), static_cast<std::uint32_t>(val), false)
#define FAST_WRITE64(addr, val)                                                                              \
    Ps2Write64(rdram, ctx, runtime, static_cast<std::uint32_t>(addr), static_cast<std::uint64_t>(val), false)
#define FAST_WRITE128(addr, val)                                                                             \
    Ps2Write128(rdram, ctx, runtime, static_cast<std::uint32_t>(addr), (val), false)

// ---------------------------------------------------------------------------------------------------------------
// General-purpose registers ($zero reads as 0 and ignores writes)
// ---------------------------------------------------------------------------------------------------------------

inline std::uint32_t Ps2GprU32(const R5900Context* ctx, int reg) noexcept {
    return reg == 0 ? 0u : static_cast<std::uint32_t>(PS2_EXTRACT_EPI32_0(ctx->r[reg]));
}
inline std::int32_t Ps2GprS32(const R5900Context* ctx, int reg) noexcept {
    return reg == 0 ? 0 : PS2_EXTRACT_EPI32_0(ctx->r[reg]);
}
inline std::uint64_t Ps2GprU64(const R5900Context* ctx, int reg) noexcept {
    return reg == 0 ? 0u : static_cast<std::uint64_t>(PS2_EXTRACT_EPI64_0(ctx->r[reg]));
}
inline std::int64_t Ps2GprS64(const R5900Context* ctx, int reg) noexcept {
    return reg == 0 ? 0 : PS2_EXTRACT_EPI64_0(ctx->r[reg]);
}
inline __m128i Ps2GprVec(const R5900Context* ctx, int reg) noexcept {
    return reg == 0 ? _mm_setzero_si128() : ctx->r[reg];
}

// Replaces the low 64 bits of a register and keeps the upper 64 (the MMI half), as the 64-bit EE writes do.
inline void Ps2SetGprLow64(R5900Context* ctx, int reg, __m128i new_low) noexcept {
    if (reg != 0) {
        ctx->r[reg] = _mm_castpd_si128(_mm_move_sd(_mm_castsi128_pd(ctx->r[reg]), _mm_castsi128_pd(new_low)));
    }
}
inline void Ps2SetGprS64(R5900Context* ctx, int reg, std::int64_t value) noexcept {
    Ps2SetGprLow64(ctx, reg, _mm_cvtsi64_si128(value));
}
inline void Ps2SetGprVec(R5900Context* ctx, int reg, __m128i value) noexcept {
    if (reg != 0) {
        ctx->r[reg] = value;
    }
}

#define GPR_U32(ctx_ptr, reg_idx) Ps2GprU32((ctx_ptr), (reg_idx))
#define GPR_S32(ctx_ptr, reg_idx) Ps2GprS32((ctx_ptr), (reg_idx))
#define GPR_U64(ctx_ptr, reg_idx) Ps2GprU64((ctx_ptr), (reg_idx))
#define GPR_S64(ctx_ptr, reg_idx) Ps2GprS64((ctx_ptr), (reg_idx))
#define GPR_VEC(ctx_ptr, reg_idx) Ps2GprVec((ctx_ptr), (reg_idx))

// 32-bit results are sign-extended to 64 bits except for SET_GPR_ZE32.
#define SET_GPR_U32(ctx_ptr, reg_idx, val)                                                                   \
    Ps2SetGprS64((ctx_ptr), (reg_idx), static_cast<std::int64_t>(static_cast<std::int32_t>(val)))
#define SET_GPR_S32(ctx_ptr, reg_idx, val)                                                                   \
    Ps2SetGprS64((ctx_ptr), (reg_idx), static_cast<std::int64_t>(static_cast<std::int32_t>(val)))
#define SET_GPR_ZE32(ctx_ptr, reg_idx, val)                                                                  \
    Ps2SetGprS64((ctx_ptr), (reg_idx), static_cast<std::int64_t>(static_cast<std::uint32_t>(val)))
#define SET_GPR_U64(ctx_ptr, reg_idx, val) Ps2SetGprS64((ctx_ptr), (reg_idx), static_cast<std::int64_t>(val))
#define SET_GPR_S64(ctx_ptr, reg_idx, val) SET_GPR_U64(ctx_ptr, reg_idx, val)
#define SET_GPR_VEC(ctx_ptr, reg_idx, val) Ps2SetGprVec((ctx_ptr), (reg_idx), (val))
