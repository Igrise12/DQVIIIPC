// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// The ABI headers in process: memory routing (FAST_* included), the GPR accessors, the integer, MMI and FPU
// helpers this project changed from upstream, and the by-name forwarders. Compiled with the project's
// warnings, so a macro expansion that warns fails the build here first.
#include "ps2_log.h"
#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "support/fake_host.h"

#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

using dq8::abi_test::Access;
using dq8::abi_test::FakeHost;
using dq8::abi_test::NamedCall;

namespace {

// Gives a test body the three names generated code has in scope.
class Guest {
  protected:
    FakeHost host;
    std::uint8_t* rdram = host.ram_base();
    R5900Context* ctx = host.ctx.get();
    PS2Runtime* runtime = &host.runtime;
};

std::uint32_t lane(__m128i v, int index) {
    return static_cast<std::uint32_t>(PS2_EXTRACT_EPI32(v, index));
}

std::uint32_t bits(float f) {
    return std::bit_cast<std::uint32_t>(f);
}

// QFSRV reference: bit i of the result is bit i + sa of rs:rt (rt low), zero past bit 255.
std::array<std::uint8_t, 16> qfsrv_reference(__m128i rs, __m128i rt, std::uint32_t sa) {
    std::array<std::uint8_t, 32> joined{};
    std::memcpy(joined.data(), &rt, 16);
    std::memcpy(joined.data() + 16, &rs, 16);
    std::array<std::uint8_t, 16> out{};
    for (std::uint32_t i = 0; i < 128; ++i) {
        const std::uint32_t from = i + sa;
        if (from < 256 && ((joined[from / 8] >> (from % 8)) & 1u) != 0) {
            out[i / 8] = static_cast<std::uint8_t>(out[i / 8] | (1u << (i % 8)));
        }
    }
    return out;
}

} // namespace

TEST_CASE_METHOD(Guest, "READ and WRITE keep RAM and its mirrors in place", "[abi][macros]") {
    WRITE32(0x00100000u, 0x11223344u);
    CHECK(READ32(0x00100000u) == 0x11223344u);
    CHECK(READ8(0x00100001u) == 0x33u);
    CHECK(READ16(0x00100002u) == 0x1122u);
    CHECK(READ32(0x80100000u) == 0x11223344u); // kseg0
    CHECK(READ32(0xA0100000u) == 0x11223344u); // kseg1
    CHECK(READ32(0x20100000u) == 0x11223344u); // uncached mirror

    // An access that runs past the end of RAM wraps byte by byte, as upstream's fast path does.
    WRITE64(0x01FFFFFCu, 0x8877665544332211ull);
    CHECK(READ32(0x01FFFFFCu) == 0x44332211u);
    CHECK(READ32(0x00000000u) == 0x88776655u);
    CHECK(READ64(0x01FFFFFCu) == 0x8877665544332211ull);

    const __m128i v = _mm_set_epi32(4, 3, 2, 1);
    WRITE128(0x00200000u, v);
    CHECK(lane(READ128(0x00200000u), 2) == 3u);
    CHECK(host.accesses.empty());
}

TEST_CASE_METHOD(Guest, "special addresses go to the host, FAST_* included", "[abi][macros]") {
    WRITE32(0x70000010u, 0xAABBCCDDu);      // scratchpad
    FAST_WRITE32(0x11000010u, 0x01020304u); // VU0 micro memory: upstream's FAST_WRITE32 writes RAM here
    FAST_WRITE16(0x1100C000u, 0x0506u);     // VU1 data, which upstream's predicate misses as well
    FAST_WRITE8(0x00300000u, 0x7Fu);        // plain RAM stays on the fast path
    host.special_values[0xB0003000u] = 0x42u;
    CHECK(FAST_READ32(0xB0003000u) == 0x42u); // kseg1 view of an EE register
    CHECK(READ8(0x00300000u) == 0x7Fu);

    const std::vector<Access> expected{{true, 4, 0x70000010u, 0xAABBCCDDu},
                                       {true, 4, 0x11000010u, 0x01020304u},
                                       {true, 2, 0x1100C000u, 0x0506u},
                                       {false, 4, 0xB0003000u, 0x42u}};
    CHECK(host.accesses == expected);
    CHECK(Ps2FastRead32(rdram, 0x01000010u) == 0u); // the RAM alias of 0x11000010 is untouched
}

TEST_CASE_METHOD(Guest, "GPR accessors sign-extend, keep the upper half and ignore $zero", "[abi][macros]") {
    SET_GPR_U32(ctx, 4, 0x80000000u);
    CHECK(GPR_U64(ctx, 4) == 0xFFFFFFFF80000000ull);
    CHECK(GPR_S32(ctx, 4) == INT32_MIN);
    CHECK(GPR_S64(ctx, 4) == -2147483648LL);

    SET_GPR_ZE32(ctx, 5, 0x80000000u);
    CHECK(GPR_U64(ctx, 5) == 0x80000000ull);

    SET_GPR_VEC(ctx, 6, _mm_set_epi64x(7, 8));
    SET_GPR_U64(ctx, 6, 9u);
    CHECK(GPR_U64(ctx, 6) == 9u);
    CHECK(PS2_EXTRACT_EPI64_1(GPR_VEC(ctx, 6)) == 7);

    SET_GPR_U32(ctx, 0, 1u);
    SET_GPR_VEC(ctx, 0, _mm_set1_epi32(-1));
    CHECK(GPR_U32(ctx, 0) == 0u);
    CHECK(GPR_U64(ctx, 0) == 0u);
    CHECK(lane(GPR_VEC(ctx, 0), 3) == 0u);
    CHECK(getRegU32(ctx, 4) == 0x80000000u);
}

TEST_CASE("integer helpers", "[abi][macros]") {
    std::uint32_t result = 0;
    bool overflow = false;
    ADD32_OV(0x7FFFFFFFu, 1, result, overflow);
    CHECK(overflow);
    CHECK(result == 0x80000000u);
    ADD32_OV(5u, -3, result, overflow);
    CHECK_FALSE(overflow);
    CHECK(result == 2u);
    SUB32_OV(0x80000000u, 1u, result, overflow);
    CHECK(overflow);
    CHECK(result == 0x7FFFFFFFu);
    SUB32_OV(1u, 2u, result, overflow);
    CHECK_FALSE(overflow);
    CHECK(result == 0xFFFFFFFFu);

    CHECK(ADD32(0xFFFFFFFFu, 2u) == 1u);
    CHECK(SRA32(0x80000000u, 4) == 0xF8000000u);
    CHECK(SRL32(0x80000000u, 4) == 0x08000000u);
    CHECK(SLT32(0xFFFFFFFFu, 0u) == 1u);
    CHECK(SLTU32(0xFFFFFFFFu, 0u) == 0u);
    CHECK(NOR32(0x0F0F0F0Fu, 0x00FF00FFu) == 0xF000F000u);

    CHECK(ps2_plzcw32(0u) == 31u);
    CHECK(ps2_plzcw32(1u) == 30u);
    CHECK(ps2_plzcw32(0xFFFFFFFFu) == 31u);
    CHECK(ps2_plzcw32(0x80000000u) == 0u);
    CHECK(ps2_plzcw32(0xF0000000u) == 3u);
    CHECK(ps2_clz32(0x00010000u) == 15u);
    CHECK(Ps2HiLoToU64(0x123456789ull, 0xABCDEF0100000002ull) == 0x2345678900000002ull);
    CHECK(Ps2SignExt32ToU64(0x80000000u) == 0xFFFFFFFF80000000ull);
}

TEST_CASE("MMI helpers", "[abi][macros]") {
    const __m128i a = _mm_set_epi32(INT32_MAX, -5, 1, INT32_MIN);
    const __m128i b = _mm_set_epi32(1, -6, 2, -1);
    const __m128i sum = PS2_PADDSW(a, b);
    CHECK(lane(sum, 0) == 0x80000000u); // saturated low
    CHECK(lane(sum, 1) == 3u);
    CHECK(lane(sum, 2) == static_cast<std::uint32_t>(-11));
    CHECK(lane(sum, 3) == 0x7FFFFFFFu); // saturated high
    const __m128i diff = PS2_PSUBSW(a, b);
    CHECK(lane(diff, 0) == 0x80000001u);
    CHECK(lane(diff, 3) == 0x7FFFFFFEu);

    CHECK(lane(ps2_paddu32(_mm_set1_epi32(-1), _mm_set1_epi32(1)), 2) == 0xFFFFFFFFu);
    CHECK(lane(ps2_psubu32(_mm_set1_epi32(1), _mm_set1_epi32(2)), 1) == 0u);
    CHECK(lane(PS2_PNOR(_mm_setzero_si128(), _mm_setzero_si128()), 0) == 0xFFFFFFFFu);
    CHECK(lane(PS2_PSRAVW(_mm_set1_epi32(INT32_MIN), _mm_set1_epi32(33)), 0) == 0xC0000000u);
    CHECK(lane(PS2_PSLLVW(_mm_set1_epi32(1), _mm_set1_epi32(31)), 2) == 0x80000000u);
    CHECK(lane(PS2_PPACW(_mm_set_epi32(8, 7, 6, 5), _mm_set_epi32(4, 3, 2, 1)), 2) == 5u);

    const __m128i rs = _mm_set_epi64x(0x1F1E1D1C1B1A1918LL, 0x1716151413121110LL);
    const __m128i rt = _mm_set_epi64x(0x0F0E0D0C0B0A0908LL, static_cast<long long>(0x8706050403020180ull));
    for (const std::uint32_t sa :
         {0u, 1u, 8u, 12u, 63u, 64u, 68u, 120u, 127u, 128u, 136u, 131u, 200u, 255u, 256u}) {
        const __m128i got = PS2_QFSRV(rs, rt, sa);
        std::array<std::uint8_t, 16> got_bytes{};
        std::memcpy(got_bytes.data(), &got, 16);
        INFO("sa = " << sa);
        CHECK(got_bytes == qfsrv_reference(rs, rt, sa));
    }
}

TEST_CASE("FPU macros follow the PS2 float rules", "[abi][macros]") {
    const float max = std::bit_cast<float>(0x7FFFFFFFu); // a NaN on the host, the largest value on the PS2
    // Through a volatile so MSVC cannot constant-fold the division (C4723 under /WX); guest code divides by a
    // register, never by a literal zero.
    volatile float zero = 0.0f;
    CHECK(bits(FPU_DIV_S(1.0f, zero)) == 0x7FFFFFFFu);
    CHECK(bits(FPU_DIV_S(-1.0f, zero)) == 0xFFFFFFFFu);
    CHECK(bits(FPU_ADD_S(max, max)) == 0x7FFFFFFFu);
    CHECK(bits(FPU_MUL_S(max, -2.0f)) == 0xFFFFFFFFu);
    CHECK(bits(FPU_ADD_S(1.5f, 2.25f)) == bits(3.75f));
    CHECK(bits(FPU_SQRT_S(-4.0f)) == bits(2.0f)); // sqrt(|x|)
    CHECK(bits(FPU_ABS_S(-0.5f)) == bits(0.5f));
    CHECK(FPU_C_LT_S(1.0f, 2.0f));
    CHECK_FALSE(FPU_C_F_S(1.0f, 2.0f));
    CHECK(FPU_TRUNC_W_S(-2.75f) == -2);
    CHECK(FPU_CVT_S_W(-3) == -3.0f);

    const __m128 sum = PS2_VADD(_mm_set_ps(0.0f, 1.0f, max, 2.0f), _mm_set_ps(0.0f, 2.0f, max, 3.0f));
    float out[4];
    std::memcpy(out, &sum, sizeof(out));
    CHECK(bits(out[0]) == bits(5.0f));
    CHECK(bits(out[1]) == 0x7FFFFFFFu);
    CHECK(bits(out[2]) == bits(3.0f));
}

TEST_CASE_METHOD(Guest, "named calls reach the host by kind and name", "[abi][forwarders]") {
    PS_LOG_ENTRY("a test");
    ps2_stubs::puts(rdram, ctx, runtime);
    ps2_syscalls::FlushCache(rdram, ctx, runtime);
    ps2_stubs::sceSifLoadElfPart(rdram, ctx, runtime);    // listed twice in PS2_STUB_LIST
    ps2_syscalls::sceSifLoadElfPart(rdram, ctx, runtime); // and in PS2_SYSCALL_LIST
    namespace stubs =
        ps2_stubs; // the spelled-out form is a generated-code marker for the no-game-data checker
    stubs::TODO_NAMED("skipped_function", rdram, ctx, runtime);
    stubs::TODO(rdram, ctx, runtime);

    const std::vector<NamedCall> expected{{DQ8_CALL_STUB, "puts"},
                                          {DQ8_CALL_SYSCALL, "FlushCache"},
                                          {DQ8_CALL_STUB, "sceSifLoadElfPart"},
                                          {DQ8_CALL_SYSCALL, "sceSifLoadElfPart"},
                                          {DQ8_CALL_TODO, "skipped_function"},
                                          {DQ8_CALL_TODO, "TODO"}};
    CHECK(host.calls == expected);
}

TEST_CASE("every listed name has a forwarder", "[abi][forwarders]") {
    // Completing the type behind each name proves the whole list compiles as call targets.
    std::size_t syscalls = 0;
    std::size_t stubs = 0;
#define DQ8_COUNT_SYSCALL(name)                                                                              \
    static_assert(sizeof(ps2_syscalls::name) == 1);                                                          \
    ++syscalls;
#define DQ8_COUNT_STUB(name)                                                                                 \
    static_assert(sizeof(ps2_stubs::name) == 1);                                                             \
    ++stubs;
    PS2_SYSCALL_LIST(DQ8_COUNT_SYSCALL)
    PS2_STUB_LIST(DQ8_COUNT_STUB)
#undef DQ8_COUNT_SYSCALL
#undef DQ8_COUNT_STUB
    CHECK(syscalls > 100u);
    CHECK(stubs > 500u);
}
