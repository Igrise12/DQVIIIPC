// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The SPU2 device (spu2.h) on a FakeMachine and a FakeIopHost: nothing scheduled before first use, the
// first use and the block cadence, manual DMA in both directions with PCSX2's chunk timing and IRQA hits,
// AutoDMA with its deferred completion, IRQs at the exact sample (checked against the chip stepped one
// sample at a time), the stops, and the same output, RAM, state, IRQs and completions however advance_to
// calls are split.
#include "runtime/ee/guest_stop.h"
#include "runtime/spu2/spu2_chip.h"
#include "test_spu2_rig.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

using namespace dq8::test::spu2;
using dq8::rt::IopAccess;
using dq8::rt::IopDmaRequest;
using dq8::rt::IopIrq;

namespace {

constexpr std::uint32_t chcr_write = 0x01000201;
constexpr std::uint32_t chcr_read = 0x01000200;
constexpr Cycles per_word = 192; // CostModel::spu2_dma_per_halfword

// BCR for `words` 16-bit words in blocks of 16 32-bit words.
constexpr std::uint32_t bcr_for(std::uint32_t words) {
    return ((words / 32) << 16) | 16;
}

std::vector<std::uint16_t> sample_words(std::uint32_t seed, bool loop) {
    Lcg lcg(seed);
    std::vector<std::uint16_t> words;
    const std::uint16_t end_flags = loop ? std::uint16_t{flag_loop_end | flag_loop} : flag_loop_end;
    for (const std::uint16_t flags : {std::uint16_t{0}, flag_loop_start, std::uint16_t{0}, end_flags}) {
        const auto b = adpcm_block(1, 2, flags, random_nibbles(lcg));
        words.insert(words.end(), b.begin(), b.end());
    }
    return words;
}

void voice(Rig& r, std::uint32_t core, std::uint32_t v, std::uint32_t address, std::uint16_t pitch,
           Cycles at) {
    r.write32(voice_addr(core, v, 0), address, at);
    r.write(voice_param(core, v, 0), 0x3FFF, at);
    r.write(voice_param(core, v, 1), 0x2FFF, at);
    r.write(voice_param(core, v, 2), pitch, at);
    r.write(voice_param(core, v, 3), 0x000F, at);
    r.write(voice_param(core, v, 4), 0x0000, at);
}

void outputs(Rig& r, Cycles at) {
    for (std::uint32_t c = 0; c < 2; ++c) {
        r.write(ext_reg(c, 0), 0x3FFF, at);
        r.write(ext_reg(c, 1), 0x3FFF, at);
    }
    r.write(core_reg(1, mmix), 0xFFC, at);
}

std::string reset_ram_sha1() {
    std::vector<std::uint8_t> bytes(2u << 20, 0);
    for (const std::uint32_t base : {0x2800u, 0xE870u}) {
        for (std::uint32_t i = 0; i < 16; ++i) {
            bytes[base * 2 + i] = 0x07;
        }
    }
    return dq8::core::to_hex(dq8::core::Sha1::of(bytes));
}

} // namespace

TEST_CASE("spu2 device: untouched, it schedules nothing and holds its reset state", "[spu2]") {
    Rig r;
    const dq8::rt::IopDeviceMapping m = r.spu2.mapping();
    CHECK(m.base == 0x1F900000u);
    CHECK(m.size == 0x800u);
    CHECK(m.device == &r.spu2);
    CHECK(m.dma == &r.spu2);
    CHECK(m.client == &r.spu2);
    CHECK(m.dma_channels == std::vector<std::uint32_t>{4, 7});
    CHECK(r.machine.events().empty());
    // Reads do not start the chip; odd widths and offsets are refused.
    CHECK(r.read(core_reg(1, statx), 50'000) == 0x80);
    std::uint32_t value = 0;
    CHECK_FALSE(r.spu2.read(core_reg(0, attr), 1, value, IopAccess{60'000, 0}));
    CHECK_FALSE(r.spu2.write(core_reg(0, attr), 4, 0, IopAccess{60'000, 0}));
    CHECK_FALSE(r.spu2.write(0x801, 2, 0, IopAccess{60'000, 0}));
    CHECK_FALSE(r.spu2.started());
    CHECK(r.machine.events().empty());
    CHECK(r.output.frames() == 0);
    CHECK(r.spu2.ram_sha1() == reset_ram_sha1());
}

TEST_CASE("spu2 device: the first write starts the clock, with the samples before it silent", "[spu2]") {
    Rig r;
    const Cycles t0 = 100 * sample + 17;
    r.write(core_reg(0, attr), 0x8000, t0);
    CHECK(r.spu2.started());
    CHECK(r.spu2.samples() == 100);
    CHECK(r.output.frames() == 100);
    // One event, a block of samples ahead.
    REQUIRE(r.machine.events().size() == 1);
    CHECK(*r.machine.events().next_time() == (100 + 32) * sample);
    r.run(132 * sample);
    CHECK(r.spu2.samples() == 132);
    CHECK(*r.machine.events().next_time() == (132 + 32) * sample);
    r.spu2.advance_to(10'000 * sample + sample - 1);
    CHECK(r.spu2.samples() == 10'000);
    CHECK(r.frames.size() == 10'000);
    CHECK(r.spu2.late_accesses() == 0);
    // An access from before what the chip has processed is counted (and applies at the chip's time).
    r.write(core_reg(0, attr), 0x8000, 9'000 * sample);
    CHECK(r.spu2.late_accesses() == 1);
}

TEST_CASE("spu2 device: manual DMA write moves chunks on PCSX2's timing and raises IRQA hits", "[spu2]") {
    Rig r;
    std::vector<std::uint16_t> words(1024);
    for (std::size_t i = 0; i < words.size(); ++i) {
        words[i] = static_cast<std::uint16_t>(0x1000 + i);
    }
    r.put_iop(0x1000, words);
    const Cycles t0 = 10 * sample;
    r.write(core_reg(0, attr), 0x8070, t0); // IRQ enable, DMA mode
    r.write32(core_reg(0, tsa_hi), 0x30000, t0);
    r.write32(core_reg(0, irqa_hi), 0x30000 + 500, t0);
    const Cycles t1 = t0 + 1000;
    r.run(t1);
    const dq8::rt::IopDmaStart s = r.spu2.dma_start(IopDmaRequest{4, 0x1000, bcr_for(1024), chcr_write, t1});
    CHECK_FALSE(s.deferred);
    CHECK(s.done_at == t1 + 1024 * per_word);
    // Chunks of min(R, 256 + R / 24) words: 298, 286, 274, 166. A write lands at the start of its chunk.
    CHECK(r.ram(0x30000) == 0x1000);
    CHECK(r.ram(0x30000 + 297) == 0x1000 + 297);
    CHECK(r.ram(0x30000 + 298) == 0);
    CHECK((r.read(core_reg(0, statx), t1) & 0x480) == 0x400);
    r.run(t1 + 298 * per_word);
    CHECK(r.ram(0x30000 + 298) == 0x1000 + 298);
    CHECK(r.host.irqs.empty());
    // IRQA lies in the second chunk: the hit is raised when that chunk's time is up.
    r.run(t1 + 584 * per_word);
    REQUIRE(r.host.irqs.size() == 1);
    CHECK(r.host.irqs[0] == std::pair{IopIrq::spu2, t1 + 584 * per_word});
    CHECK((r.read(irqinfo, t1 + 584 * per_word) & 4) == 4);
    r.run(s.done_at);
    CHECK(r.ram(0x30000 + 1023) == 0x1000 + 1023);
    CHECK((r.read(core_reg(0, statx), s.done_at) & 0x480) == 0x80);
    CHECK((std::uint32_t{r.read(core_reg(0, tsa_hi), s.done_at)} << 16 |
           r.read(core_reg(0, tsa_lo), s.done_at)) == 0x30400);
    CHECK(r.host.irqs.size() == 1);
    CHECK(r.host.dma_done.empty()); // the IOP completes a manual transfer at done_at itself
}

TEST_CASE("spu2 device: a DMA does not hit an IRQA at its first word", "[spu2]") {
    Rig r;
    r.put_iop(0x1000, std::vector<std::uint16_t>(64, 0x5555));
    const Cycles t0 = 10 * sample;
    r.write(core_reg(1, attr), 0x8040, t0);
    r.write32(core_reg(1, tsa_hi), 0x40000, t0);
    r.write32(core_reg(1, irqa_hi), 0x40000, t0);
    const auto s = r.spu2.dma_start(IopDmaRequest{7, 0x1000, bcr_for(64), chcr_write, t0});
    r.run(s.done_at + 10 * sample);
    CHECK(r.host.irqs.empty());
    CHECK(r.ram(0x40000) == 0x5555);
}

TEST_CASE("spu2 device: manual DMA read fills IOP RAM a chunk at a time", "[spu2]") {
    Rig r;
    std::vector<std::uint16_t> words(320);
    for (std::size_t i = 0; i < words.size(); ++i) {
        words[i] = static_cast<std::uint16_t>(0xA000 + i);
    }
    const Cycles t0 = 3 * sample;
    r.poke(1, 0x50000, words, t0);
    r.write32(core_reg(1, tsa_hi), 0x50000, t0);
    const auto s = r.spu2.dma_start(IopDmaRequest{7, 0x2000, bcr_for(320), chcr_read, t0});
    CHECK(s.done_at == t0 + 321 * per_word);
    CHECK(r.iop_word(0x2000) == 0); // reads land at the end of their chunk
    r.run(t0 + 256 * per_word);
    CHECK(r.iop_word(0x2000) == 0xA000);
    CHECK(r.iop_word(0x2000 + 2 * 255) == 0xA000 + 255);
    CHECK(r.iop_word(0x2000 + 2 * 256) == 0);
    r.run(t0 + 320 * per_word);
    CHECK(r.iop_word(0x2000 + 2 * 319) == 0xA000 + 319);
    CHECK((r.read(core_reg(1, statx), t0 + 320 * per_word) & 0x400) == 0x400);
    r.run(s.done_at);
    CHECK((r.read(core_reg(1, statx), s.done_at) & 0x400) == 0);
}

TEST_CASE("spu2 device: AutoDMA feeds the input area at the sample rate and completes deferred", "[spu2]") {
    Rig r;
    // Two blocks of 256 left and 256 right samples.
    std::vector<std::uint16_t> stream;
    for (std::uint32_t block = 0; block < 2; ++block) {
        for (std::uint32_t side = 0; side < 2; ++side) {
            for (std::uint32_t i = 0; i < 256; ++i) {
                stream.push_back(static_cast<std::uint16_t>((block + 1) * 0x1000 + side * 0x800 + i));
            }
        }
    }
    r.put_iop(0x8000, stream);
    const Cycles t0 = 7 * sample;
    outputs(r, t0);
    r.write(core_reg(0, admas), 1, t0);
    // Start between two refill points (OutPos 0x40).
    const Cycles t1 = (7 + 0x40) * sample;
    r.run(t1);
    const auto s = r.spu2.dma_start(IopDmaRequest{4, 0x8000, bcr_for(1024), chcr_write, t1});
    CHECK(s.deferred);
    // The first block goes at once into the second half of the double buffer.
    CHECK(r.ram(0x2100) == 0x1000);
    CHECK(r.ram(0x2300) == 0x1800);
    CHECK(r.ram(0x2000) == 0);
    // At OutPos 0x100 the first half takes block 2; three samples later the MADR walk ends: completion.
    const Cycles done = (7 + 260) * sample;
    r.run(done - 1);
    CHECK(r.host.dma_done.empty());
    CHECK(r.ram(0x2000) == 0x2000);
    r.run(done);
    REQUIRE(r.host.dma_done.size() == 1);
    CHECK(r.host.dma_done[0] == std::pair{std::uint32_t{4}, done});
    r.spu2.advance_to((7 + 768) * sample);
    // Input through core 0 (input volume 0x7FFF, master 0x7FFE) into core 1 (external 0x7FFF, master 0x7FFE).
    const auto through = [](std::int32_t x) {
        x = (0x7FFF * x) >> 15;
        x = (0x7FFE * x) >> 15;
        x = (0x7FFF * x) >> 15;
        return static_cast<std::int16_t>((0x7FFE * x) >> 15);
    };
    const std::size_t first = 7;
    for (std::uint32_t i = 0; i < 256; ++i) {
        INFO("sample " << i);
        REQUIRE(r.frames[first + i].left == 0);
        REQUIRE(r.frames[first + 256 + i].left == through(0x1000 + static_cast<std::int32_t>(i)));
        REQUIRE(r.frames[first + 256 + i].right == through(0x1800 + static_cast<std::int32_t>(i)));
        REQUIRE(r.frames[first + 512 + i].left == through(0x2000 + static_cast<std::int32_t>(i)));
    }
    // The input ran dry at OutPos 0: ADMAS shows it.
    CHECK(r.read(core_reg(0, admas), (7 + 768) * sample) == 0xFFFD);
}

TEST_CASE("spu2 device: IRQs arrive at the exact sample, as the chip stepped alone raises them", "[spu2]") {
    const auto words = sample_words(41, true);
    const std::uint32_t address = 0x60000;
    // The chip alone, one sample at a time: when do the IRQs come?
    const auto reference = [&](std::uint32_t irqa) {
        dq8::rt::spu2::Chip chip;
        chip.write(core_reg(0, tsa_hi), static_cast<std::uint16_t>(address >> 16));
        chip.write(core_reg(0, tsa_lo), static_cast<std::uint16_t>(address));
        for (const std::uint16_t w : words) {
            chip.write(core_reg(0, data), w);
        }
        chip.write(voice_addr(0, 5, 0), static_cast<std::uint16_t>(address >> 16));
        chip.write(voice_addr(0, 5, 1), static_cast<std::uint16_t>(address));
        chip.write(voice_param(0, 5, 2), 0x0C00);
        chip.write(voice_param(0, 5, 3), 0x000F);
        chip.write(core_reg(0, irqa_hi), static_cast<std::uint16_t>(irqa >> 16));
        chip.write(core_reg(0, irqa_lo), static_cast<std::uint16_t>(irqa));
        chip.write(core_reg(0, attr), 0x8040);
        chip.write(core_reg(0, kon), 1u << 5);
        std::vector<std::uint64_t> hits;
        for (std::uint64_t n = 0; n < 3000; ++n) {
            (void)chip.mix();
            if (chip.take_irqs() != 0) {
                hits.push_back(n);
                // Acknowledge at once (IRQ off and on again).
                chip.write(core_reg(0, attr), 0x8000);
                chip.write(core_reg(0, attr), 0x8040);
            }
        }
        return hits;
    };
    const auto device = [&](std::uint32_t irqa) {
        Rig r;
        const Cycles t0 = 40 * sample;
        r.poke(0, address, words, t0);
        r.write32(voice_addr(0, 5, 0), address, t0);
        r.write(voice_param(0, 5, 2), 0x0C00, t0);
        r.write(voice_param(0, 5, 3), 0x000F, t0);
        r.write32(core_reg(0, irqa_hi), irqa, t0);
        r.write(core_reg(0, attr), 0x8040, t0);
        r.write(core_reg(0, kon), 1u << 5, t0);
        std::vector<std::uint64_t> hits;
        std::size_t seen = 0;
        while (const auto next = r.machine.events().next_time()) {
            if (*next > (40 + 3000) * sample) {
                break;
            }
            r.run(*next);
            while (seen < r.host.irqs.size()) {
                const Cycles at = r.host.irqs[seen++].second;
                hits.push_back(at / sample - 40 - 1);
                r.write(core_reg(0, attr), 0x8000, at);
                r.write(core_reg(0, attr), 0x8040, at);
            }
        }
        return hits;
    };
    for (const std::uint32_t irqa : {address + 8 * 2 + 3, address + 8 * 3, 0x800u + 0x40, 0x2000u + 0x123}) {
        INFO("IRQA " << irqa);
        const auto expected = reference(irqa);
        CHECK_FALSE(expected.empty());
        CHECK(device(irqa) == expected);
    }
}

TEST_CASE("spu2 device: output, RAM, state, IRQs and completions do not depend on advance_to splits",
          "[spu2]") {
    struct Result {
        std::string frames;
        std::string audio_hash;
        std::string ram;
        std::string state;
        std::vector<std::pair<IopIrq, Cycles>> irqs;
        std::vector<std::pair<std::uint32_t, Cycles>> dma_done;
        std::vector<Cycles> done_at;
        std::uint64_t late = 0;
        bool operator==(const Result&) const = default;
    };
    const auto scenario = [](const std::function<void(Rig&, Cycles, Cycles)>& between) {
        Rig r;
        std::vector<std::uint16_t> stream(1024);
        for (std::size_t i = 0; i < stream.size(); ++i) {
            stream[i] = static_cast<std::uint16_t>(i * 37);
        }
        r.put_iop(0x8000, stream);
        r.put_iop(0x4000, sample_words(5, true));
        Result res;
        Cycles t = 5 * sample + 100;
        const auto at = [&](Cycles dt) {
            between(r, t, t + dt);
            t += dt;
            r.run(t);
        };
        at(0);
        r.poke(0, 0x10000, sample_words(1, true), t);
        r.poke(0, 0x20000, sample_words(2, false), t);
        outputs(r, t);
        voice(r, 0, 0, 0x10000, 0x1000, t);
        voice(r, 0, 1, 0x20000, 0x0800, t);
        voice(r, 1, 5, 0x10000, 0x1800, t);
        r.write(core_reg(0, kon), 0x3, t);
        at(700 * sample + 33);
        r.write(core_reg(1, kon), 1u << 5, t);
        at(1000 * sample);
        r.write32(core_reg(0, irqa_hi), 0x10000 + 8 * 2 + 3, t);
        r.write(core_reg(0, attr), 0x8040, t);
        at(2000 * sample + 5);
        r.write32(core_reg(1, tsa_hi), 0x30000, t);
        res.done_at.push_back(r.spu2.dma_start(IopDmaRequest{7, 0x4000, bcr_for(32), chcr_write, t}).done_at);
        at(300 * sample + 1);
        r.write32(voice_addr(1, 6, 0), 0x30000, t);
        r.write(voice_param(1, 6, 2), 0x1234, t);
        r.write(voice_param(1, 6, 3), 0x000F, t);
        r.write(core_reg(1, kon), 1u << 6, t);
        r.write(voice_param(0, 0, 2), 0x1234, t);
        at(1500 * sample + 77);
        r.write(core_reg(0, attr), 0x8000, t);
        r.write(core_reg(0, attr), 0x8040, t);
        r.write(core_reg(0, admas), 1, t);
        res.done_at.push_back(
            r.spu2.dma_start(IopDmaRequest{4, 0x8000, bcr_for(1024), chcr_write, t}).done_at);
        at(2500 * sample + 9);
        r.write(core_reg(0, koff), 0x1, t);
        at(1000 * sample);
        r.spu2.advance_to(t);
        res.frames = frames_sha1(r.frames);
        res.audio_hash = r.output.hash_hex();
        res.ram = r.spu2.ram_sha1();
        res.state = r.spu2.state_sha1();
        res.irqs = r.host.irqs;
        res.dma_done = r.host.dma_done;
        res.late = r.spu2.late_accesses();
        return res;
    };
    const Result events_only = scenario([](Rig&, Cycles, Cycles) {});
    const Result every_sample = scenario([](Rig& r, Cycles from, Cycles to) {
        for (Cycles b = (from / sample + 1) * sample; b < to; b += sample) {
            r.run(b);
            r.spu2.advance_to(b);
        }
    });
    Lcg lcg(99);
    const Result random_splits = scenario([&lcg](Rig& r, Cycles from, Cycles to) {
        for (Cycles b = from + 1 + lcg.next() % (40 * sample); b < to; b += 1 + lcg.next() % (40 * sample)) {
            r.run(b);
            r.spu2.advance_to(b);
        }
    });
    CHECK_FALSE(events_only.irqs.empty());
    CHECK(events_only.dma_done.size() == 1);
    CHECK(events_only.late == 0);
    CHECK(events_only == every_sample);
    CHECK(events_only == random_splits);
    CHECK(events_only.frames == "a24d94ee40e8c0ed831ee14a8017e257e82fd2b7");
    CHECK(events_only.ram == "29ae887bc67d0134f3f514482ec4717a68353a86");
}

TEST_CASE("spu2 device: unmodelled modes and DMA requests stop the machine", "[spu2]") {
    Rig r;
    CHECK_THROWS_AS(r.write(0x7C0, 0x0004, 1000), dq8::rt::GuestStop);
    Rig r2;
    CHECK_THROWS_AS(r2.write(core_reg(1, admas), 0x7FFF, 1000), dq8::rt::GuestStop);
    Rig r3;
    CHECK_THROWS_AS(r3.spu2.dma_start(IopDmaRequest{5, 0, bcr_for(32), chcr_write, 0}), dq8::rt::GuestStop);
    CHECK_THROWS_AS(r3.spu2.dma_start(IopDmaRequest{4, 0, bcr_for(32), 0x01000401, 0}), dq8::rt::GuestStop);
}

TEST_CASE("spu2 device: stopping a manual transfer abandons its remaining chunks", "[spu2]") {
    Rig r;
    r.put_iop(0x1000, std::vector<std::uint16_t>(1024, 0x7777));
    const Cycles t0 = 2 * sample;
    r.write32(core_reg(0, tsa_hi), 0x30000, t0);
    const auto s = r.spu2.dma_start(IopDmaRequest{4, 0x1000, bcr_for(1024), chcr_write, t0});
    r.run(t0 + 100 * per_word);
    r.spu2.dma_stop(4, t0 + 100 * per_word);
    CHECK((r.read(core_reg(0, statx), t0 + 100 * per_word) & 0x400) == 0);
    r.run(s.done_at + sample);
    CHECK(r.ram(0x30000 + 297) == 0x7777);
    CHECK(r.ram(0x30000 + 298) == 0); // the second chunk never came
}

TEST_CASE("spu2 device: the status handshakes a sound library polls", "[spu2]") {
    Rig r;
    Cycles t = 4 * sample;
    // Power-on: STATX shows only the DMA-ready bit. A core reset (ATTR 0, then on) leaves STATX's low eleven
    // bits clear, which is what a driver waits for.
    CHECK(r.read(core_reg(0, statx), t) == 0x80);
    for (std::uint32_t c = 0; c < 2; ++c) {
        r.write(core_reg(c, attr), 0x0000, t);
        r.write(core_reg(c, attr), 0x8000, t);
        CHECK((r.read(core_reg(c, statx), t) & 0x7FF) == 0);
    }
    // DMA write mode: ready; a transfer: busy, not ready; done: ready again.
    r.write(core_reg(0, attr), 0x8020, t);
    CHECK((r.read(core_reg(0, statx), t) & 0x480) == 0x80);
    r.put_iop(0x3000, std::vector<std::uint16_t>(64, 0x1111));
    r.write32(core_reg(0, tsa_hi), 0x5000, t);
    const auto s = r.spu2.dma_start(IopDmaRequest{4, 0x3000, bcr_for(64), chcr_write, t});
    CHECK((r.read(core_reg(0, statx), t) & 0x480) == 0x400);
    CHECK((r.read(core_reg(0, statx), s.done_at - 1) & 0x480) == 0x400);
    CHECK((r.read(core_reg(0, statx), s.done_at) & 0x480) == 0x80);
    // ENDX: KON clears the started voice's bit at the next sample.
    t = s.done_at + 10 * sample;
    CHECK((r.read(core_reg(0, endx), t) & 1) == 1);
    r.write(core_reg(0, kon), 1, t);
    CHECK((r.read(core_reg(0, endx), t) & 1) == 1);
    CHECK((r.read(core_reg(0, endx), t + sample) & 1) == 0);
}
