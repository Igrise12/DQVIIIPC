// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/hash.h"
#include "runtime/iop/hw/iop_hw.h"
#include "runtime/machine/clock.h"
#include "runtime/platform/audio.h"
#include "runtime/spu2/spu2.h"
#include "support/fake_iop_host.h"
#include "support/fake_machine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

// The SPU2 on a FakeMachine and a FakeIopHost (no IOP interpreter): register offsets, ADPCM block authoring,
// and a capture of every frame the chip mixes. Tests drive time like the IOP would: events due before an
// access run first.
namespace dq8::test::spu2 {

using rt::Cycles;
inline constexpr Cycles sample = rt::ee_cycles_per_spu2_sample;

// Register offsets from 0x1F900000.
constexpr std::uint32_t voice_param(std::uint32_t core, std::uint32_t voice, std::uint32_t param) {
    return (core << 10) + voice * 16 +
           param * 2; // 0 VOLL, 1 VOLR, 2 PITCH, 3 ADSR1, 4 ADSR2, 5 ENVX, 6/7 VOLX
}
constexpr std::uint32_t voice_addr(std::uint32_t core, std::uint32_t voice, std::uint32_t k) {
    return (core << 10) + 0x1C0 + voice * 12 + k * 2; // 0/1 SSA hi/lo, 2/3 LSAX, 4/5 NAX
}
constexpr std::uint32_t core_reg(std::uint32_t core, std::uint32_t offset) {
    return (core << 10) + offset;
}
constexpr std::uint32_t ext_reg(std::uint32_t core, std::uint32_t index) {
    return 0x760 + core * 0x28 + index * 2; // 0/1 MVOL, 2/3 EVOL, 4/5 AVOL, 6/7 BVOL, 8/9 MVOLX, 10.. reverb
}
inline constexpr std::uint32_t pmon = 0x180, non = 0x184, vmixl = 0x188, vmixel = 0x18C, vmixr = 0x190,
                               vmixer = 0x194, mmix = 0x198, attr = 0x19A, irqa_hi = 0x19C, irqa_lo = 0x19E,
                               kon = 0x1A0, koff = 0x1A4, tsa_hi = 0x1A8, tsa_lo = 0x1AA, data = 0x1AC,
                               admas = 0x1B0, esa_hi = 0x2E0, esa_lo = 0x2E2, eea = 0x33C, endx = 0x340,
                               statx = 0x344, irqinfo = 0x7C2;

inline constexpr std::uint16_t flag_loop_end = 1, flag_loop = 2, flag_loop_start = 4;

// One 16-byte ADPCM block: the header word (shift, filter, flags) and 28 nibbles, lowest nibble first.
inline std::array<std::uint16_t, 8> adpcm_block(std::uint32_t shift, std::uint32_t filter,
                                                std::uint16_t flags,
                                                std::span<const std::uint8_t, 28> nibbles) {
    std::array<std::uint16_t, 8> b{};
    b[0] = static_cast<std::uint16_t>((std::uint32_t{flags} << 8) | ((filter & 0xFu) << 4) | (shift & 0xFu));
    for (std::size_t i = 0; i < 28; ++i) {
        b[1 + i / 4] = static_cast<std::uint16_t>(b[1 + i / 4] | ((nibbles[i] & 0xFu) << (4 * (i % 4))));
    }
    return b;
}

// A small deterministic generator for test data.
class Lcg {
  public:
    explicit Lcg(std::uint32_t seed) : state_(seed) {}
    std::uint32_t next() {
        state_ = state_ * 1664525u + 1013904223u;
        return state_ >> 8;
    }

  private:
    std::uint32_t state_;
};

inline std::array<std::uint8_t, 28> random_nibbles(Lcg& lcg) {
    std::array<std::uint8_t, 28> n{};
    for (std::uint8_t& x : n) {
        x = static_cast<std::uint8_t>(lcg.next() & 0xF);
    }
    return n;
}

class CaptureTap final : public rt::IAudioTap {
  public:
    explicit CaptureTap(std::vector<rt::StereoFrame>& frames) : frames_(frames) {}
    void write(std::span<const rt::StereoFrame> frames) override {
        frames_.insert(frames_.end(), frames.begin(), frames.end());
    }

  private:
    std::vector<rt::StereoFrame>& frames_;
};

inline std::string frames_sha1(std::span<const rt::StereoFrame> frames) {
    core::Sha1 sha;
    for (const rt::StereoFrame& f : frames) {
        const auto l = static_cast<std::uint16_t>(f.left);
        const auto r = static_cast<std::uint16_t>(f.right);
        const std::array<std::uint8_t, 4> b = {
            static_cast<std::uint8_t>(l), static_cast<std::uint8_t>(l >> 8), static_cast<std::uint8_t>(r),
            static_cast<std::uint8_t>(r >> 8)};
        sha.update(b);
    }
    return core::to_hex(sha.finish());
}

struct Rig {
    FakeMachine machine;
    FakeIopHost host{machine};
    std::vector<rt::StereoFrame> frames;
    rt::AudioOutput output{1u << 12};
    rt::Spu2 spu2{rt::Spu2Config{&output}};
    std::uint32_t pc = 0x00012340;

    Rig() {
        output.add_tap(std::make_unique<CaptureTap>(frames));
        spu2.connect(host);
        machine.attach(spu2);
    }

    // Runs the events due by `t`, as the IOP would before an access at `t`.
    void run(Cycles t) {
        machine.run_events_until(t);
        host.now = t;
    }
    void write(std::uint32_t offset, std::uint16_t value, Cycles at) {
        run(at);
        const bool handled = spu2.write(offset, 2, value, rt::IopAccess{at, pc}); // may throw GuestStop
        REQUIRE(handled);
    }
    std::uint16_t read(std::uint32_t offset, Cycles at) {
        run(at);
        std::uint32_t value = 0;
        REQUIRE(spu2.read(offset, 2, value, rt::IopAccess{at, pc}));
        return static_cast<std::uint16_t>(value);
    }
    void write32(std::uint32_t offset_hi, std::uint32_t value, Cycles at) {
        write(offset_hi, static_cast<std::uint16_t>(value >> 16), at);
        write(offset_hi + 2, static_cast<std::uint16_t>(value), at);
    }
    // Writes words into sound RAM through TSA and the data register.
    void poke(std::uint32_t core, std::uint32_t address, std::span<const std::uint16_t> words, Cycles at) {
        write32(core_reg(core, tsa_hi), address, at);
        for (const std::uint16_t w : words) {
            write(core_reg(core, data), w, at);
        }
    }
    void put_iop(std::uint32_t address, std::span<const std::uint16_t> words) {
        for (std::size_t i = 0; i < words.size(); ++i) {
            host.ram().data()[address + 2 * i] = static_cast<std::uint8_t>(words[i]);
            host.ram().data()[address + 2 * i + 1] = static_cast<std::uint8_t>(words[i] >> 8);
        }
    }
    std::uint16_t iop_word(std::uint32_t address) {
        return static_cast<std::uint16_t>(host.ram().data()[address] | (host.ram().data()[address + 1] << 8));
    }
    std::uint16_t ram(std::uint32_t address) const { return spu2.chip().ram()[address]; }
};

} // namespace dq8::test::spu2
