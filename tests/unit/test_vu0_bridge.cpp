// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// VU0 micro mode from EE code (vu/vu0_runner.h, M4 day 0): VCALLMS runs a VU0 program on the calling
// thread's VU0 registers in its R5900Context, and the cycles it reports are the program's own.
#include "dq8/abi/ps2recomp/r5900_context.h"
#include "runtime/vu/vu0_runner.h"
#include "support/ee_rig.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

using namespace dq8::rt;
using dq8::test::EeRig;
using dq8::test::put32;

namespace {

constexpr std::uint32_t lower_nop = 0x8000033Cu;
constexpr std::uint32_t upper_nop = 0x000002FFu;
constexpr std::uint32_t e_bit = 1u << 30;

// Upper VADD.xyzw fd, fs, ft.
constexpr std::uint32_t vadd(unsigned fd, unsigned fs, unsigned ft) {
    return (0xFu << 21) | (ft << 16) | (fs << 11) | (fd << 6) | 0x28u;
}

// Writes {lower, upper} pairs at byte address `at` in VU0 micro memory.
void load_program(VuUnits& vu, std::uint32_t at, std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint8_t> bytes;
    for (const std::uint32_t w : words) {
        put32(bytes, w);
    }
    vu.write_micro(0, at, bytes);
}

std::array<float, 4> vf(const R5900Context& ctx, unsigned reg) {
    std::array<float, 4> out{};
    _mm_storeu_ps(out.data(), ctx.vu0_vf[reg]);
    return out;
}

} // namespace

TEST_CASE("vu0 bridge: VCALLMS runs on the context's VU0 registers and reports the program's cycles",
          "[vu][vu0]") {
    EeRig rig;
    REQUIRE(rig.machine.vu0() == &rig.vu()); // registered at attach
    // At 0x40: VADD vf3, vf1, vf2 with the E bit, then the delay-slot pair.
    load_program(rig.vu(), 0x40, {lower_nop, vadd(3, 1, 2) | e_bit, lower_nop, upper_nop});
    R5900Context ctx{};
    ctx.vu0_vf[1] = _mm_set_ps(4.0f, 3.0f, 2.0f, 1.0f);
    ctx.vu0_vf[2] = _mm_set_ps(40.0f, 30.0f, 20.0f, 10.0f);
    ctx.vi[5] = 9;

    const std::uint64_t first = rig.machine.vu0()->run_from_ee(ctx, 0x40);
    CHECK(vf(ctx, 3) == std::array<float, 4>{11.0f, 22.0f, 33.0f, 44.0f});
    CHECK(ctx.vi[5] == 9);                                             // untouched registers survive
    CHECK(vf(ctx, 0) == std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}); // VF0 is fixed
    CHECK(first > 0u);
    CHECK(rig.vu().stats(0).programs == 1u);

    // A second run of the same program costs the same: the count is per program, not the running total.
    ctx.vu0_vf[1] = _mm_set_ps(1.0f, 1.0f, 1.0f, 1.0f);
    const std::uint64_t second = rig.machine.vu0()->run_from_ee(ctx, 0x40);
    CHECK(second == first);
    CHECK(vf(ctx, 3) == std::array<float, 4>{11.0f, 21.0f, 31.0f, 41.0f});
    CHECK(rig.vu().stats(0).cycles == first + second);
}

TEST_CASE("vu0: a new context's VF0 is (0, 0, 0, 1) before any micro program runs", "[vu][vu0]") {
    // Macro-mode code reads VF0 like any register (VMADDw by vf0w scales by 1): a zero VF0 made the field
    // culling reject every object (M4 bring-up B1).
    const R5900Context ctx{};
    CHECK(vf(ctx, 0) == std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
    R5900Context reused{};
    reused.vu0_vf[0] = _mm_setzero_ps();
    reused = R5900Context();
    CHECK(vf(reused, 0) == std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
}

TEST_CASE("vu0 bridge: two contexts keep their own VU0 registers", "[vu][vu0]") {
    EeRig rig;
    load_program(rig.vu(), 0, {lower_nop, vadd(3, 1, 1) | e_bit, lower_nop, upper_nop});
    R5900Context a{};
    R5900Context b{};
    a.vu0_vf[1] = _mm_set_ps(1.0f, 1.0f, 1.0f, 1.0f);
    b.vu0_vf[1] = _mm_set_ps(5.0f, 5.0f, 5.0f, 5.0f);
    b.vu0_vf[7] = _mm_set_ps(7.0f, 7.0f, 7.0f, 7.0f);
    rig.machine.vu0()->run_from_ee(a, 0);
    rig.machine.vu0()->run_from_ee(b, 0);
    CHECK(vf(a, 3) == std::array<float, 4>{2.0f, 2.0f, 2.0f, 2.0f});
    CHECK(vf(b, 3) == std::array<float, 4>{10.0f, 10.0f, 10.0f, 10.0f});
    CHECK(vf(a, 7) == std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}); // b's registers never leak into a
}
