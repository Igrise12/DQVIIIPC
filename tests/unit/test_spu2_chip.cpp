// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The SPU2 chip without time (spu2::Chip): the power-on state, the register file, voices playing authored
// ADPCM with their loop flags, envelopes and volume sweeps against values derived by hand from the
// documented rates, noise, pitch modulation, the dry/wet and core 0 -> core 1 routing, and reverb on and
// off. Mixed output is pinned to golden hashes.
#include "runtime/spu2/spu2_chip.h"
#include "test_spu2_rig.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <vector>

using namespace dq8::test::spu2;
using dq8::rt::StereoFrame;
using Chip = dq8::rt::spu2::Chip;

namespace {

void poke(Chip& chip, std::uint32_t core, std::uint32_t address, std::span<const std::uint16_t> words) {
    chip.write(core_reg(core, tsa_hi), static_cast<std::uint16_t>(address >> 16));
    chip.write(core_reg(core, tsa_lo), static_cast<std::uint16_t>(address));
    for (const std::uint16_t w : words) {
        chip.write(core_reg(core, data), w);
    }
}

void set_ssa(Chip& chip, std::uint32_t core, std::uint32_t voice, std::uint32_t address) {
    chip.write(voice_addr(core, voice, 0), static_cast<std::uint16_t>(address >> 16));
    chip.write(voice_addr(core, voice, 1), static_cast<std::uint16_t>(address));
}

std::uint32_t read32(const Chip& chip, std::uint32_t offset_hi) {
    return (std::uint32_t{chip.read(offset_hi)} << 16) | chip.read(offset_hi + 2);
}

std::vector<StereoFrame> mix(Chip& chip, std::size_t n) {
    std::vector<StereoFrame> out;
    for (std::size_t i = 0; i < n; ++i) {
        const auto s = chip.mix();
        out.push_back(StereoFrame{static_cast<std::int16_t>(s.left), static_cast<std::int16_t>(s.right)});
    }
    return out;
}

// A three-block sample at `address`: A (plain), B (loop start), C (loop end, with `loop` or without).
void author_sample(Chip& chip, std::uint32_t address, bool loop, std::uint32_t seed = 7) {
    Lcg lcg(seed);
    std::vector<std::uint16_t> words;
    const std::uint16_t end_flags = loop ? std::uint16_t{flag_loop_end | flag_loop} : flag_loop_end;
    for (const std::uint16_t flags : {std::uint16_t{0}, flag_loop_start, end_flags}) {
        const auto b = adpcm_block(2, 1, flags, random_nibbles(lcg));
        words.insert(words.end(), b.begin(), b.end());
    }
    poke(chip, 0, address, words);
}

// Every output of both cores on (core 1 also takes core 0 dry), master volumes up.
void open_outputs(Chip& chip) {
    for (std::uint32_t c = 0; c < 2; ++c) {
        chip.write(ext_reg(c, 0), 0x3FFF);
        chip.write(ext_reg(c, 1), 0x3FFF);
    }
    chip.write(core_reg(1, mmix), 0xFFC);
}

// Sets a voice up and keys it on. KON is a latch until the next sample: keying several voices for the same
// sample takes one write with all their bits (`key` false here).
void play(Chip& chip, std::uint32_t voice, std::uint32_t address, std::uint16_t adsr1, std::uint16_t adsr2,
          bool key = true) {
    set_ssa(chip, 0, voice, address);
    chip.write(voice_param(0, voice, 0), 0x3FFF);
    chip.write(voice_param(0, voice, 1), 0x3FFF);
    chip.write(voice_param(0, voice, 2), 0x1000);
    chip.write(voice_param(0, voice, 3), adsr1);
    chip.write(voice_param(0, voice, 4), adsr2);
    if (key) {
        chip.write(core_reg(0, kon), static_cast<std::uint16_t>(1u << voice));
    }
}

} // namespace

TEST_CASE("spu2 chip: power-on state", "[spu2]") {
    Chip chip;
    CHECK(chip.read(core_reg(0, statx)) == 0x80);
    CHECK(chip.read(core_reg(1, statx)) == 0x80);
    CHECK(chip.read(core_reg(0, endx)) == 0xFFFF); // ENDX 0xFFFFFF: low half first
    CHECK(chip.read(core_reg(0, endx + 2)) == 0xFF);
    CHECK(chip.read(core_reg(0, mmix)) == 0xFF0);
    CHECK(chip.read(core_reg(1, mmix)) == 0xFFC);
    CHECK(read32(chip, core_reg(0, irqa_hi)) == 0x800);
    CHECK(read32(chip, core_reg(0, esa_hi)) == 0xEFFF8);
    CHECK(read32(chip, core_reg(1, esa_hi)) == 0xFFFF8);
    CHECK(chip.read(voice_param(0, 5, 2)) == 0x3FFF);
    CHECK(read32(chip, voice_addr(1, 23, 0)) == 0x2800);
    CHECK(read32(chip, voice_addr(1, 23, 4)) == 0x2801);
    CHECK(chip.ram()[0x2800] == 0x0707);
    CHECK(chip.ram()[0xE877] == 0x0707);
    CHECK(chip.read(ext_reg(0, 4)) == 0x7FFF); // AVOLL
    CHECK(chip.read(ext_reg(1, 6)) == 0x7FFF); // BVOLL
    CHECK(chip.read(irqinfo) == 0);
}

TEST_CASE("spu2 chip: register file semantics", "[spu2]") {
    Chip chip;
    // Voice addresses: SSA and LSAX keep 8-word alignment and 20 bits; NAX always points past a header.
    chip.write(voice_addr(0, 3, 0), 0xFFFF);
    chip.write(voice_addr(0, 3, 1), 0x1237);
    CHECK(read32(chip, voice_addr(0, 3, 0)) == 0xF1230);
    chip.write(voice_addr(0, 3, 5), 0x4560);
    CHECK(read32(chip, voice_addr(0, 3, 4)) == 0x04561);
    // IRQA and TSA take what is written.
    chip.write(core_reg(1, irqa_hi), 0x0001);
    chip.write(core_reg(1, irqa_lo), 0x2345);
    CHECK(read32(chip, core_reg(1, irqa_hi)) == 0x12345);
    // KON latches until the next sample, which clears ENDX for the voices it starts.
    chip.write(core_reg(0, kon), 0x0003);
    CHECK(chip.read(core_reg(0, kon)) == 3);
    CHECK((chip.read(core_reg(0, endx)) & 3) == 3);
    (void)chip.mix();
    CHECK(chip.read(core_reg(0, kon)) == 0);
    CHECK((chip.read(core_reg(0, endx)) & 3) == 0);
    // Any write to an ENDX half clears that half.
    chip.write(core_reg(1, endx + 2), 0x1234);
    CHECK(chip.read(core_reg(1, endx + 2)) == 0);
    CHECK(chip.read(core_reg(1, endx)) == 0xFFFF);
    // A fixed volume reads back as written; its level is twice the value.
    chip.write(voice_param(0, 0, 0), 0x2000);
    CHECK(chip.read(voice_param(0, 0, 0)) == 0x2000);
    CHECK(chip.read(voice_param(0, 0, 6)) == 0x4000);
    // MVOLX is read-only; ENVX is writable.
    chip.write(ext_reg(0, 0), 0x1000);
    chip.write(ext_reg(0, 8), 0x7777);
    CHECK(chip.read(ext_reg(0, 8)) == 0x2000);
    chip.write(voice_param(1, 2, 5), 0x1234);
    CHECK(chip.read(voice_param(1, 2, 5)) == 0x1234);
    // ATTR: IRQ enable, then the IRQ status bit stays until the IRQ is disabled.
    chip.write(core_reg(0, attr), 0x8040);
    CHECK(chip.read(core_reg(0, attr)) == 0x8040);
    chip.write(irqinfo, 0x0004);
    CHECK(chip.read(irqinfo) == 4);
    chip.write(core_reg(0, attr), 0x8000);
    CHECK(chip.read(irqinfo) == 0);
    // DMA mode in ATTR raises STATX's ready bit; clearing it drops the bit.
    chip.write(core_reg(1, attr), 0x0030);
    CHECK((chip.read(core_reg(1, statx)) & 0x80) == 0x80);
    chip.write(core_reg(1, attr), 0x0000);
    CHECK((chip.read(core_reg(1, statx)) & 0x80) == 0);
    // EEA sets only the upper half of the work area's end.
    chip.write(core_reg(0, eea), 0x000A);
    CHECK(read32(chip, core_reg(0, eea)) == 0xAFFFF);
    // Core 1's DIFF_L_SRC and DIFF_R_SRC trade places on writes, as in PCSX2.
    chip.write(core_reg(1, 0x324), 0x0000);
    chip.write(core_reg(1, 0x326), 0x0111);
    CHECK(chip.read(core_reg(1, 0x32A)) == 0x0111);
    CHECK(chip.read(core_reg(1, 0x326)) == 0);
    chip.write(core_reg(0, 0x326), 0x0222);
    CHECK(chip.read(core_reg(0, 0x326)) == 0x0222);
    // Unknown registers keep their value.
    chip.write(0x7D0, 0xBEEF);
    CHECK(chip.read(0x7D0) == 0xBEEF);
    chip.write(core_reg(1, 0x1B2), 0xCAFE);
    CHECK(chip.read(core_reg(1, 0x1B2)) == 0xCAFE);
    CHECK(chip.read(core_reg(0, 0x1B2)) == 0);
}

TEST_CASE("spu2 chip: modes we do not model stop", "[spu2]") {
    Chip chip;
    CHECK_THROWS_AS(chip.write(0x7C0, 0x0004), dq8::rt::spu2::Unsupported);
    CHECK_THROWS_AS(chip.write(core_reg(0, admas), 0x7FFF), dq8::rt::spu2::Unsupported);
    chip.write(0x7C0, 0x0020); // PCM out: no effect on the mix
}

TEST_CASE("spu2 chip: loop flags drive ENDX, NAX and the end of a one-shot voice", "[spu2]") {
    SECTION("looping") {
        Chip chip;
        open_outputs(chip);
        author_sample(chip, 0x10000, true);
        play(chip, 0, 0x10000, 0x000F, 0x0000);
        const auto out = mix(chip, 28 * 3 + 4);
        CHECK((chip.read(core_reg(0, endx)) & 1) == 1); // passed the end of block C
        // After block C the voice continues from block B (its loop start), not from A.
        const std::uint32_t nax = read32(chip, voice_addr(0, 0, 4));
        CHECK(nax > 0x10008);
        CHECK(nax < 0x10010);
        CHECK(read32(chip, voice_addr(0, 0, 2)) == 0x10008);
        CHECK(chip.read(voice_param(0, 0, 5)) != 0);
        CHECK(frames_sha1(mix(chip, 1000)) == "2a47d1fb7af2a2b533492894c2b70b00254dc750");
        bool any = false;
        for (const StereoFrame& f : out) {
            any = any || f.left != 0;
        }
        CHECK(any);
    }
    SECTION("one shot") {
        Chip chip;
        open_outputs(chip);
        author_sample(chip, 0x10000, false);
        play(chip, 0, 0x10000, 0x000F, 0x0000);
        (void)mix(chip, 28 * 3 + 8);
        CHECK((chip.read(core_reg(0, endx)) & 1) == 1);
        CHECK(chip.read(voice_param(0, 0, 5)) == 0); // stopped: ENVX 0
        // A stopped voice keeps walking its addresses (IRQs on them still fire).
        const std::uint32_t nax = read32(chip, voice_addr(0, 0, 4));
        (void)mix(chip, 40);
        CHECK(read32(chip, voice_addr(0, 0, 4)) != nax);
        // Key on again: ENDX clears and the voice sounds.
        chip.write(core_reg(0, kon), 1);
        (void)mix(chip, 4);
        CHECK((chip.read(core_reg(0, endx)) & 1) == 0);
        CHECK(chip.read(voice_param(0, 0, 5)) != 0);
    }
}

TEST_CASE("spu2 chip: envelopes follow the documented rates", "[spu2]") {
    Chip chip;
    author_sample(chip, 0x10000, true);
    // Linear attack, rate 0 (+0x3800 a sample), decay rate 0 to sustain level 15, linear sustain up, linear
    // release rate 0 (-0x4000 a sample).
    play(chip, 0, 0x10000, 0x000F, 0x0000);
    std::vector<int> envx;
    for (int i = 0; i < 6; ++i) {
        (void)chip.mix();
        envx.push_back(chip.read(voice_param(0, 0, 5)));
    }
    // Attack 0x3800, 0x7000, 0x7FFF (clamped, phase ends); decay: 0x7FFF - 0x4000 = 0x3FFF (exponential at
    // full level), below the sustain level 0x8000; sustain rises again by 0x3800.
    CHECK(envx == std::vector<int>{0x3800, 0x7000, 0x7FFF, 0x3FFF, 0x77FF, 0x7FFF});
    chip.write(core_reg(0, koff), 1);
    (void)chip.mix();
    CHECK(chip.read(voice_param(0, 0, 5)) == 0x3FFF);
    (void)chip.mix();
    CHECK(chip.read(voice_param(0, 0, 5)) == 0);

    // Exponential attack slows to a quarter of the rate above 0x6000.
    Chip exp_chip;
    author_sample(exp_chip, 0x10000, true);
    play(exp_chip, 0, 0x10000, 0x800F, 0x0000);
    std::vector<int> e;
    for (int i = 0; i < 6; ++i) {
        (void)exp_chip.mix();
        e.push_back(exp_chip.read(voice_param(0, 0, 5)));
    }
    CHECK(e == std::vector<int>{0x3800, 0x7000, 0x7000, 0x7000, 0x7000, 0x7FFF});
}

TEST_CASE("spu2 chip: volume sweeps", "[spu2]") {
    Chip chip;
    // Linear increase, rate 0: +0x3800 a sample from the current level, clamped.
    chip.write(voice_param(0, 4, 0), 0x8000);
    std::vector<int> v;
    for (int i = 0; i < 4; ++i) {
        (void)chip.mix();
        v.push_back(chip.read(voice_param(0, 4, 6)));
    }
    CHECK(v == std::vector<int>{0x3800, 0x7000, 0x7FFF, 0x7FFF});
    // Linear decrease (bit 13), rate 0: towards 0.
    chip.write(voice_param(0, 4, 0), 0xA000);
    std::vector<int> d;
    for (int i = 0; i < 4; ++i) {
        (void)chip.mix();
        d.push_back(chip.read(voice_param(0, 4, 6)));
    }
    CHECK(d == std::vector<int>{0x3FFF, 0x0000, 0x0000, 0x0000});
    // Master volume sweeps the same way.
    chip.write(ext_reg(1, 0), 0x8000);
    (void)chip.mix();
    CHECK(chip.read(ext_reg(1, 8)) == 0x3800);
}

TEST_CASE("spu2 chip: noise, pitch modulation and the output areas", "[spu2]") {
    Chip chip;
    open_outputs(chip);
    author_sample(chip, 0x10000, true, 11);
    author_sample(chip, 0x20000, true, 12);
    chip.write(core_reg(0, attr), 0x3F00); // the fastest noise clock
    chip.write(core_reg(0, non), 0x0004);  // voice 2 plays noise
    chip.write(core_reg(0, pmon), 0x0002); // voice 1 is modulated by voice 0
    play(chip, 0, 0x10000, 0x000F, 0x0000, false);
    play(chip, 1, 0x20000, 0x000F, 0x0000, false);
    play(chip, 2, 0x20000, 0x000F, 0x0000, false);
    chip.write(core_reg(0, kon), 0x0007);
    const auto out = mix(chip, 2000);
    CHECK(frames_sha1(out) == "ff64cfb1a76d79e49ce53588159728046e55cec7");
    // The noise voice sounds without reading its sample; the modulated voice runs at another rate.
    CHECK(chip.read(voice_param(0, 2, 5)) == 0x7FFF);
    CHECK(read32(chip, voice_addr(0, 1, 4)) != read32(chip, voice_addr(0, 0, 4)));
    // Voice 1's output and core 0's dry mix land in sound RAM at OutPos.
    const std::uint32_t last = (2000 - 1) & 0x1FF;
    CHECK(chip.ram()[0x400 + last] != 0);
    CHECK(chip.ram()[0x1000 + last] != 0);
    CHECK(chip.ram()[0x800 + last] != 0);
}

TEST_CASE("spu2 chip: reverb on and off", "[spu2]") {
    const auto run = [](bool fx) {
        Chip chip;
        open_outputs(chip);
        author_sample(chip, 0x10000, true, 21);
        // A work area of 0x10000 words at 0x80000, an authored set of delays (in words) and gains.
        chip.write(core_reg(1, esa_hi), 0x0008);
        chip.write(core_reg(1, esa_lo), 0x0000);
        chip.write(core_reg(1, eea), 0x0008);
        const std::uint32_t delays[22] = {0x0100, 0x0080, 0x1000, 0x1100, 0x0F00, 0x0E80, 0x0D00, 0x0C80,
                                          0x0FF0, 0x10F0, 0x2000, 0x2100, 0x0B00, 0x0A80, 0x0900, 0x0880,
                                          0x1FF0, 0x20F0, 0x3000, 0x3100, 0x4000, 0x4100};
        for (std::uint32_t i = 0; i < 22; ++i) {
            chip.write(core_reg(1, 0x2E4 + 4 * i), 0);
            chip.write(core_reg(1, 0x2E6 + 4 * i), static_cast<std::uint16_t>(delays[i]));
        }
        const std::uint16_t gains[10] = {0x7000, 0x5000, 0x4800, 0x4000, 0x3800,
                                         0x6000, 0x5800, 0x5000, 0x7FFF, 0x7FFF};
        for (std::uint32_t i = 0; i < 10; ++i) {
            chip.write(ext_reg(1, 10 + i), gains[i]);
        }
        chip.write(ext_reg(1, 2), 0x6000); // EVOL
        chip.write(ext_reg(1, 3), 0x6000);
        chip.write(core_reg(1, mmix), 0xFFF); // core 0 into core 1, dry and wet
        chip.write(core_reg(1, attr), fx ? 0x0080 : 0x0000);
        play(chip, 0, 0x10000, 0x000F, 0x0000);
        const auto out = mix(chip, 4800);
        std::uint64_t written = 0;
        for (std::uint32_t a = 0x80000; a < 0x90000; ++a) {
            if (chip.ram()[a] != 0) {
                ++written;
            }
        }
        return std::pair{frames_sha1(out), written};
    };
    const auto [off, off_written] = run(false);
    const auto [on, on_written] = run(true);
    CHECK(off_written == 0); // without FxEnable the work area is left alone
    CHECK(on_written > 1000);
    CHECK(on != off);
    CHECK(off == "e5da3d61d484ca0d968ad3c8c75a9839f19d0c06");
    CHECK(on == "91ce422658527dc1b1db809adbd3cdc4b2f1dc75");
}

TEST_CASE("spu2 chip: core 0 reaches the output only through core 1's external input", "[spu2]") {
    Chip chip;
    author_sample(chip, 0x10000, true, 31);
    for (std::uint32_t c = 0; c < 2; ++c) {
        chip.write(ext_reg(c, 0), 0x3FFF);
        chip.write(ext_reg(c, 1), 0x3FFF);
    }
    play(chip, 0, 0x10000, 0x000F, 0x0000);
    // Power-on gates: core 1 takes core 0 only into its effect path, whose volume is 0.
    const auto silent = mix(chip, 200);
    bool any = false;
    for (const StereoFrame& f : silent) {
        any = any || f.left != 0 || f.right != 0;
    }
    CHECK_FALSE(any);
    chip.write(core_reg(1, mmix), 0x00C); // dry external input on
    const auto out = mix(chip, 200);
    // The output is core 0's committed output (0x800/0xA00) scaled by core 1's external and master volumes.
    const std::uint32_t pos = (400 - 1) & 0x1FF;
    const auto ext = static_cast<std::int16_t>(chip.ram()[0x800 + pos]);
    const std::int32_t scaled = (0x7FFF * std::int32_t{ext}) >> 15;
    CHECK(out.back().left == static_cast<std::int16_t>((0x7FFE * scaled) >> 15));
    CHECK(ext != 0);
}
