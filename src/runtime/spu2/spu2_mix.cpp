// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Adapted from PCSX2 (PCSX2/pcsx2, 3e29183a37e7, tag v2.7.524), PCSX2 Dev Team: pcsx2/SPU2/Mixer.cpp (ADPCM
// decoding, the decode FIFO, pitch, interpolation, noise, the voice and core mixes, spu2Mix), ADSR.cpp
// (envelopes and volume sweeps), ReadInput.cpp (V_Core::ReadInput), Dma.cpp (AutoDMAReadBuffer), Reverb.cpp
// (DoReverb) and ReverbResample.cpp (the 39-tap resampling filters, with the arithmetic of their AVX2
// variant), spu2sys.cpp (SetIrqCall, TimeUpdate's KOFF/KON order).
#include "runtime/spu2/spu2_chip.h"
#include "runtime/spu2/spu2_tables.h"

#include <algorithm>
#include <array>

namespace dq8::rt::spu2 {

namespace {

constexpr std::int8_t flag_loop_end = 1 << 0;
constexpr std::int8_t flag_loop = 1 << 1;
constexpr std::int8_t flag_loop_start = 1 << 2;

constexpr std::int32_t clamp16(std::int32_t x) {
    return std::clamp<std::int32_t>(x, -0x8000, 0x7FFF);
}
constexpr Stereo clamp16(Stereo s) {
    return {clamp16(s.left), clamp16(s.right)};
}
// (volume * data) >> 15 on 32-bit integers, wrapping like the original's int arithmetic would on its hosts.
constexpr std::int32_t mul15(std::int32_t a, std::int32_t b) {
    const auto product = static_cast<std::uint32_t>(static_cast<std::int64_t>(a) * b);
    return static_cast<std::int32_t>(product) >> 15;
}
constexpr Stereo volume(Stereo s, std::int32_t left, std::int32_t right) {
    return {mul15(left, s.left), mul15(right, s.right)};
}
constexpr Stereo add(Stereo a, Stereo b) {
    return {a.left + b.left, a.right + b.right};
}

// The 39-tap half-band filter of the reverb's 2:1 resampling; the upsampler uses twice the weights.
constexpr std::array<std::int16_t, 48> filter_down = {
    -1,    0, 2,     0, -10,  0, 35,   0, -103, 0, 266,  0, -616, 0, 1332, 0, -2960, 0, 10246, 16384,
    10246, 0, -2960, 0, 1332, 0, -616, 0, 266,  0, -103, 0, 35,   0, -10,  0, 2,     0, -1};

constexpr std::array<std::int16_t, 48> make_filter_up() {
    std::array<std::int16_t, 48> up{};
    for (std::size_t i = 0; i < 39; ++i) {
        up[i] = static_cast<std::int16_t>(std::clamp<std::int32_t>(filter_down[i] * 2, -0x8000, 0x7FFF));
    }
    return up;
}
constexpr std::array<std::int16_t, 48> filter_up = make_filter_up();

// PMULHRSW and the saturating 16-bit adds of the vector filters.
constexpr std::int16_t mulhrs(std::int16_t a, std::int16_t b) {
    return static_cast<std::int16_t>((std::int32_t{a} * b + 0x4000) >> 15);
}
constexpr std::int16_t adds(std::int32_t a, std::int32_t b) {
    return static_cast<std::int16_t>(clamp16(a + b));
}

// The filter as PCSX2's AVX2 code computes it: sixteen lanes accumulate taps i, i+16 and i+32 with rounding
// multiplies and saturating adds, the halves are added, then three horizontal saturating adds.
std::int16_t fir(const std::array<std::int16_t, 128>& buf, std::uint32_t pos,
                 const std::array<std::int16_t, 48>& coefs) {
    const std::uint32_t index = (pos - 39) & 63;
    std::array<std::int16_t, 16> acc{};
    for (std::uint32_t j = 0; j < 16; ++j) {
        std::int16_t a = mulhrs(buf[index + j], coefs[j]);
        a = adds(a, mulhrs(buf[index + j + 16], coefs[j + 16]));
        a = adds(a, mulhrs(buf[index + j + 32], coefs[j + 32]));
        acc[j] = a;
    }
    std::array<std::int16_t, 8> lane{};
    for (std::uint32_t j = 0; j < 8; ++j) {
        lane[j] = adds(acc[j], acc[j + 8]);
    }
    const std::int16_t h0 = adds(lane[0], lane[1]);
    const std::int16_t h1 = adds(lane[2], lane[3]);
    const std::int16_t h2 = adds(lane[4], lane[5]);
    const std::int16_t h3 = adds(lane[6], lane[7]);
    return adds(adds(h0, h1), adds(h2, h3));
}

} // namespace

void decode_adpcm_block(std::span<const std::uint16_t, 8> block, std::array<std::int16_t, 28>& out,
                        std::int32_t& prev1, std::int32_t& prev2) {
    const auto header = static_cast<std::int32_t>(static_cast<std::int16_t>(block[0]));
    const std::int32_t shift = (header & 0xF) + 16;
    const auto& f = adpcm_factors[static_cast<std::size_t>((header >> 4) & 0xF)];
    std::size_t k = 0;
    for (std::size_t w = 1; w < 8; ++w) {
        for (std::uint32_t nibble = 0; nibble < 4; ++nibble) {
            // The nibble at the top of a 32-bit word, shifted down arithmetically: (n << 12) >> shift.
            const auto data = static_cast<std::int32_t>(((block[w] >> (4 * nibble)) & 0xFu) << 28);
            const std::int32_t pcm = clamp16((data >> shift) + ((f[0] * prev1 + f[1] * prev2 + 32) >> 6));
            out[k++] = static_cast<std::int16_t>(pcm);
            prev2 = prev1;
            prev1 = pcm;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// Volumes and envelopes

void VolumeSlide::set(std::uint16_t v) {
    reg = v;
    if ((reg & 0x8000) == 0) {
        value = static_cast<std::int16_t>(static_cast<std::uint16_t>(v << 1));
    }
}

void VolumeSlide::update() {
    if ((reg & 0x8000) == 0) {
        return;
    }
    const std::int32_t step = reg & 3;
    const std::int32_t shift = (reg >> 2) & 0x1F;
    const bool phase = (reg & 0x1000) != 0;
    const bool decr = (reg & 0x2000) != 0;
    const bool exp = (reg & 0x4000) != 0;

    std::int32_t step_size = 7 - step;
    if (decr) {
        step_size = ~step_size;
    }
    std::uint32_t counter_inc = 0x8000u >> std::max(0, shift - 11);
    std::int32_t level_inc = step_size * (1 << std::max(0, 11 - shift));
    if (exp) {
        if (!decr && value > 0x6000) {
            counter_inc >>= 2;
        }
        if (decr) {
            level_inc = static_cast<std::int16_t>((level_inc * value) >> 15);
        }
    }
    // A rate with every bit set never moves.
    if (step != 3 && shift != 0x1F) {
        counter_inc = std::max<std::uint32_t>(1, counter_inc);
    }
    counter += counter_inc;
    if (!(exp && decr)) {
        level_inc = phase ? -level_inc : level_inc;
    }
    if (counter >= 0x8000) {
        counter = 0;
        if (!decr) {
            value = std::clamp<std::int32_t>(value + level_inc, -0x8000, 0x7FFF);
        } else {
            std::int32_t low = phase ? -0x8000 : 0;
            std::int32_t high = phase ? 0 : 0x7FFF;
            if (exp) {
                low = 0;
                high = 0x7FFF;
            }
            value = std::clamp<std::int32_t>(value + level_inc, low, high);
        }
    }
}

void Envelope::update_cache() {
    const std::uint32_t sustain_level = adsr1 & 0xF;
    const auto decay_shift = static_cast<std::uint8_t>((adsr1 >> 4) & 0xF);
    const std::uint32_t attack_step = (adsr1 >> 8) & 3;
    const auto attack_shift = static_cast<std::uint8_t>((adsr1 >> 10) & 0x1F);
    const bool attack_exp = (adsr1 & 0x8000) != 0;
    const auto release_shift = static_cast<std::uint8_t>(adsr2 & 0x1F);
    const bool release_exp = (adsr2 & 0x20) != 0;
    const std::uint32_t sustain_step = (adsr2 >> 6) & 3;
    const auto sustain_shift = static_cast<std::uint8_t>((adsr2 >> 8) & 0x1F);
    const bool sustain_decr = (adsr2 & 0x4000) != 0;
    const bool sustain_exp = (adsr2 & 0x8000) != 0;

    phases[attack] = {false, attack_exp, attack_shift, static_cast<std::int8_t>(7 - attack_step), 0x7FFF};
    phases[decay] = {true, true, decay_shift, -8, static_cast<std::int32_t>((sustain_level + 1) << 11)};
    auto sustain_step_size = static_cast<std::int8_t>(7 - sustain_step);
    if (sustain_decr) {
        sustain_step_size = static_cast<std::int8_t>(~sustain_step_size);
    }
    phases[sustain] = {sustain_decr, sustain_exp, sustain_shift, sustain_step_size, 0};
    phases[release] = {true, release_exp, release_shift, -8, 0};
}

bool Envelope::step() {
    const Phase& p = phases[phase];
    std::uint32_t counter_inc = 0x8000u >> std::max(0, p.shift - 11);
    std::int32_t level_inc = p.step * (1 << std::max(0, 11 - p.shift));
    if (p.exp) {
        if (!p.decr && value > 0x6000) {
            counter_inc >>= 2;
        }
        if (p.decr) {
            level_inc = static_cast<std::int16_t>((level_inc * value) >> 15);
        }
    }
    counter_inc = std::max<std::uint32_t>(1, counter_inc);
    counter += counter_inc;
    if (counter >= 0x8000) {
        counter = 0;
        value = std::clamp<std::int32_t>(value + level_inc, 0, 0x7FFF);
    }
    // Sustain lasts until key off or silence.
    if (phase == sustain) {
        return value != 0;
    }
    if ((!p.decr && value >= p.target) || (p.decr && value <= p.target)) {
        ++phase;
    }
    return phase <= release;
}

void Envelope::start_attack() {
    phase = attack;
    counter = 0;
    value = 0;
}

void Envelope::start_release() {
    if (phase != stopped) {
        phase = release;
        counter = 0;
    }
}

void Voice::key_on() {
    env.start_attack();
    loop_mode = 0;
    sp = 0;
    loop_flags = 0;
    next = start | 1;
    prev1 = 0;
    prev2 = 0;
    block_valid = false;
    fifo_read = 0;
    fifo_write = 0;
}

void Voice::stop() {
    env.value = 0;
    env.phase = Envelope::stopped;
}

// ---------------------------------------------------------------------------------------------------------
// IRQ

void Chip::set_irq(std::uint32_t core) {
    const auto bit = static_cast<std::uint16_t>(4u << core);
    if ((spdif_.info & bit) == 0 && cores_[core].irq_enable) {
        spdif_.info |= bit;
        irq_signals_ |= 1u << core;
    }
}

// Both cores watch every address the chip touches, whichever core touches it.
void Chip::check_irq(std::uint32_t address) {
    for (std::uint32_t i = 0; i < 2; ++i) {
        if (cores_[i].irq_enable && cores_[i].irqa == address) {
            set_irq(i);
        }
    }
}

void Chip::write_fast(std::uint32_t address, std::int32_t value) {
    check_irq(address);
    ram_[address] = static_cast<std::uint16_t>(value);
}

// ---------------------------------------------------------------------------------------------------------
// Voices

void Chip::increment_next(Voice& vc) {
    check_irq(vc.next);
    vc.next = (vc.next + 1) & address_mask;
}

void Chip::decode_samples(Core& c, std::uint32_t v) {
    Voice& vc = c.voices[v];
    // The block header is read on every sample.
    const std::uint32_t header = vc.next & 0xFFFF8u;
    check_irq(header);
    vc.loop_flags = static_cast<std::int8_t>(static_cast<std::int16_t>(ram_[header]) >> 8);
    if ((vc.loop_flags & flag_loop_start) != 0 && vc.loop_mode == 0) {
        vc.loop_start = header;
    }
    if (static_cast<std::int32_t>(vc.fifo_write - vc.fifo_read) > 12) {
        return; // enough decoded
    }
    if (vc.env.phase > Envelope::stopped) {
        if (!vc.block_valid) {
            decode_adpcm_block(std::span<const std::uint16_t, 8>(ram_.data() + (vc.next & 0xFFFF8u), 8),
                               vc.block, vc.prev1, vc.prev2);
            vc.block_valid = true;
        }
        const std::uint32_t index = ((vc.next % 8) - 1) * 4;
        for (std::uint32_t i = 0; i < 4; ++i) {
            vc.fifo[(vc.fifo_write + i) % 32] = vc.block[index + i];
        }
    }
    vc.fifo_write += 4;
    increment_next(vc);
    if ((vc.next & 7) == 0) {
        if ((vc.loop_flags & flag_loop_end) != 0) {
            c.endx |= 1u << v;
            vc.next = vc.loop_start;
            if ((vc.loop_flags & flag_loop) == 0) {
                vc.stop();
            }
        }
        increment_next(vc); // skip the next block's header
        vc.block_valid = false;
    }
}

Stereo Chip::mix_voice(Core& c, std::uint32_t v) {
    Voice& vc = c.voices[v];
    vc.vol_left.update();
    vc.vol_right.update();
    decode_samples(c, v);

    Stereo out;
    std::int32_t value = 0;
    if (vc.env.phase > Envelope::stopped) {
        if (vc.noise) {
            value = static_cast<std::int16_t>(c.noise_out);
        } else {
            const auto& w = gauss_table[static_cast<std::size_t>((vc.sp & 0x0FF0) >> 4)];
            for (std::uint32_t i = 0; i < 4; ++i) {
                value += (w[i] * vc.fifo[(vc.fifo_read + i) % 32]) >> 15;
            }
        }
        if (!vc.env.step()) {
            vc.stop();
        }
        value = mul15(vc.env.value, value);
        vc.out_x = value;
        out = {mul15(vc.vol_left.value, value), mul15(vc.vol_right.value, value)};
    }

    // Every voice keeps running whether it sounds or not, so that IRQs on its addresses fire.
    std::int32_t pitch = vc.pitch;
    if (vc.modulated && v != 0) {
        const std::int64_t m = (std::int64_t{vc.pitch} * (32768 + std::int64_t{c.voices[v - 1].out_x})) >> 15;
        pitch = static_cast<std::int32_t>(std::clamp<std::int64_t>(m, 0, 0x3FFF));
    }
    vc.sp += std::min(pitch, 0x3FFF);
    vc.fifo_read += static_cast<std::uint32_t>(vc.sp >> 12);
    vc.sp &= 0xFFF;

    // Voices 1 and 3 of each core write their output into sound RAM.
    if (v == 1) {
        write_fast((c.index == 0 ? 0x400u : 0xC00u) + out_pos_, value);
    } else if (v == 3) {
        write_fast((c.index == 0 ? 0x600u : 0xE00u) + out_pos_, value);
    }
    return out;
}

void Chip::update_noise(Core& c) {
    static constexpr std::array<std::uint8_t, 64> noise_add = {
        1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0,
        0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1};
    static constexpr std::array<std::uint16_t, 5> noise_freq_add = {0, 84, 140, 180, 210};

    const std::uint32_t level = (0x8000u >> (c.noise_clock >> 2)) << 16;
    c.noise_count += 0x10000;
    c.noise_count += noise_freq_add[c.noise_clock & 3];
    if ((c.noise_count & 0xFFFF) >= noise_freq_add[4]) {
        c.noise_count += 0x10000;
        c.noise_count -= noise_freq_add[c.noise_clock & 3];
    }
    if (c.noise_count >= level) {
        while (c.noise_count >= level) {
            c.noise_count -= level;
        }
        c.noise_out = (c.noise_out << 1) | noise_add[(c.noise_out >> 10) & 63];
    }
}

// ---------------------------------------------------------------------------------------------------------
// AutoDMA input

Stereo Chip::read_input(Core& c) {
    std::uint16_t read_index = out_pos_;
    const std::uint32_t base = 0x2000 + (c.index << 10);
    for (std::uint32_t i = 0; i < 2; ++i) {
        if (cores_[i].irq_enable && base + read_index == (cores_[i].irqa & 0xFFFFFDFFu)) {
            set_irq(i);
        }
    }
    Stereo in;
    if (c.index == 1 || (play_mode_ & 2) == 0) { // bypass mode leaves core 0's input out of the mix
        in = {static_cast<std::int16_t>(ram_[base + read_index]),
              static_cast<std::int16_t>(ram_[base + 0x200 + read_index])};
    }
    // PCSX2 walks MADR forward here; the transfer ends when the last block has been read and walked.
    if (c.input_transferred != 0) {
        c.input_transferred -= std::min<std::uint32_t>(c.input_transferred, 0x180);
        if (c.input_transferred == 0 && c.input_left == 0) {
            finish_dma(c);
            adma_done_ |= 1u << c.index;
        }
    }
    if (play_mode_ == 2 && c.index == 0) { // bitstream bypass refills twice as fast
        read_index = static_cast<std::uint16_t>((read_index * 2) & 0x1FF);
    }
    if (read_index == 0x100 || read_index == 0 || read_index == 0x80 || read_index == 0x180) {
        if (read_index == 0x100) {
            c.input_pos_write = 0;
        } else if (read_index == 0) {
            c.input_pos_write = 0x100;
        }
        if (c.input_left >= 0x100) {
            adma_read_buffer(c);
            c.adma_in_progress = true;
            if (c.input_left < 0x100) {
                c.input_left = 0;
            }
        } else if ((c.adma_ctrl & (c.index + 1)) != 0) {
            c.adma_ctrl |= 0xFFFC; // PCSX2: AutoDMACtrl |= ~3, the input ran dry
        }
    }
    return in;
}

void Chip::adma_read_buffer(Core& c) {
    if (c.input_pos_write == 0xFFFF) {
        return; // no request yet
    }
    std::uint32_t spos = c.input_pos_write & 0x100;
    bool left = (c.input_pos_write & 0x80) == 0;
    c.adma_ctrl &= 0x3;
    std::int32_t size = static_cast<std::int32_t>(std::min<std::uint32_t>(c.input_left, 0x200));
    if (!left) {
        size = std::min(size, 0x100);
    }
    const std::uint32_t base = 0x2000 + (c.index << 10);
    // Whole half blocks of 256 words (PCSX2 assumes the transfer is made of them).
    while (size > 0) {
        spos = left ? (spos & ~0x200u) : (spos | 0x200u);
        for (std::uint32_t i = 0; i < 0x100; ++i) {
            ram_[base + spos + i] = iop_word(c.adma_madr + 2 * (c.input_progress + i));
        }
        c.input_transferred += 0x200;
        c.input_left -= std::min<std::uint32_t>(c.input_left, 0x100);
        c.input_progress += 0x100;
        left = !left;
        size -= 0x100;
        c.input_pos_write += 0x80;
    }
    if ((c.input_pos_write & 0x80) == 0) {
        c.input_pos_write = 0xFFFF;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Reverb

std::uint32_t Chip::reverb_index(const Core& c, std::int32_t offset) const {
    const std::uint32_t start = c.effects_start & 0x3FFFFF;
    const std::uint32_t end = (c.effects_end & 0x3FFFFF) | 0xFFFF;
    const std::uint32_t size = end - start + 1;
    std::uint32_t x = (cycles_ >> 1) + static_cast<std::uint32_t>(offset);
    if (size != 0) {
        x %= size;
    }
    return (x + start) & address_mask;
}

Stereo Chip::do_reverb(Core& c, Stereo input) {
    if (c.effects_start >= c.effects_end) {
        return {};
    }
    input = clamp16(input);
    const std::uint32_t pos = c.revb_pos;
    c.revb_down[0][pos] = c.revb_down[0][pos | 64] = static_cast<std::int16_t>(input.left);
    c.revb_down[1][pos] = c.revb_down[1][pos | 64] = static_cast<std::int16_t>(input.right);

    const bool r = (cycles_ & 1) != 0;
    const Reverb& v = c.revb;
    const auto idx = [&](std::uint32_t offset) { return reverb_index(c, static_cast<std::int32_t>(offset)); };
    const std::uint32_t same_src = idx(r ? v.same_r_src : v.same_l_src);
    const std::uint32_t same_dst = idx(r ? v.same_r_dst : v.same_l_dst);
    const std::uint32_t same_prv = idx((r ? v.same_r_dst : v.same_l_dst) - 1);
    const std::uint32_t diff_src = idx(r ? v.diff_l_src : v.diff_r_src);
    const std::uint32_t diff_dst = idx(r ? v.diff_r_dst : v.diff_l_dst);
    const std::uint32_t diff_prv = idx((r ? v.diff_r_dst : v.diff_l_dst) - 1);
    const std::uint32_t comb1_src = idx(r ? v.comb1_r_src : v.comb1_l_src);
    const std::uint32_t comb2_src = idx(r ? v.comb2_r_src : v.comb2_l_src);
    const std::uint32_t comb3_src = idx(r ? v.comb3_r_src : v.comb3_l_src);
    const std::uint32_t comb4_src = idx(r ? v.comb4_r_src : v.comb4_l_src);
    const std::uint32_t apf1_src = idx((r ? v.apf1_r_dst : v.apf1_l_dst) - v.apf1_size);
    const std::uint32_t apf1_dst = idx(r ? v.apf1_r_dst : v.apf1_l_dst);
    const std::uint32_t apf2_src = idx((r ? v.apf2_r_dst : v.apf2_l_dst) - v.apf2_size);
    const std::uint32_t apf2_dst = idx(r ? v.apf2_r_dst : v.apf2_l_dst);

    for (std::uint32_t i = 0; i < 2; ++i) {
        const std::uint32_t a = cores_[i].irqa;
        if (c.fx_enable && cores_[i].irq_enable && a >= c.effects_start && a <= c.effects_end) {
            for (const std::uint32_t x :
                 {same_src, diff_src, same_dst, diff_dst, same_prv, diff_prv, comb1_src, comb2_src, comb3_src,
                  comb4_src, apf1_dst, apf1_src, apf2_dst, apf2_src}) {
                if (a == x) {
                    set_irq(i);
                    break;
                }
            }
        }
    }

    const auto mem = [this](std::uint32_t a) {
        return static_cast<std::int32_t>(static_cast<std::int16_t>(ram_[a]));
    };
    const std::int32_t in =
        mul15(r ? v.in_coef_r : v.in_coef_l, fir(c.revb_down[r ? 1 : 0], pos, filter_down));
    const std::int32_t same =
        mul15(v.iir_vol, in + mul15(v.wall_vol, mem(same_src)) - mem(same_prv)) + mem(same_prv);
    const std::int32_t diff =
        mul15(v.iir_vol, in + mul15(v.wall_vol, mem(diff_src)) - mem(diff_prv)) + mem(diff_prv);
    std::int32_t out = mul15(v.comb1_vol, mem(comb1_src)) + mul15(v.comb2_vol, mem(comb2_src)) +
                       mul15(v.comb3_vol, mem(comb3_src)) + mul15(v.comb4_vol, mem(comb4_src));
    const std::int32_t apf1 = out - mul15(v.apf1_vol, mem(apf1_src));
    out = mem(apf1_src) + mul15(v.apf1_vol, apf1);
    const std::int32_t apf2 = out - mul15(v.apf2_vol, mem(apf2_src));
    out = mem(apf2_src) + mul15(v.apf2_vol, apf2);
    // The effect always runs; FxEnable decides whether it writes its work area back.
    if (c.fx_enable) {
        ram_[same_dst] = static_cast<std::uint16_t>(clamp16(same));
        ram_[diff_dst] = static_cast<std::uint16_t>(clamp16(diff));
        ram_[apf1_dst] = static_cast<std::uint16_t>(clamp16(apf1));
        ram_[apf2_dst] = static_cast<std::uint16_t>(clamp16(apf2));
    }
    out = clamp16(out);
    const std::size_t rs = r ? 1 : 0;
    c.revb_up[rs][pos] = c.revb_up[rs][pos | 64] = static_cast<std::int16_t>(out);
    c.revb_up[rs ^ 1][pos] = c.revb_up[rs ^ 1][pos | 64] = 0;
    c.revb_pos = (pos + 1) & 63;
    return {fir(c.revb_up[0], c.revb_pos, filter_up), fir(c.revb_up[1], c.revb_pos, filter_up)};
}

// ---------------------------------------------------------------------------------------------------------
// The mix

Stereo Chip::mix_core(Core& c, Stereo dry, Stereo wet, Stereo input, Stereo ext) {
    c.master_left.update();
    c.master_right.update();
    update_noise(c);
    dry = clamp16(dry);
    wet = clamp16(wet);
    const std::uint32_t area = c.index == 0 ? 0x1000u : 0x1800u;
    write_fast(area + out_pos_, dry.left);
    write_fast(area + 0x200 + out_pos_, dry.right);
    write_fast(area + 0x400 + out_pos_, wet.left);
    write_fast(area + 0x600 + out_pos_, wet.right);

    const CoreGates& d = c.dry_gate;
    const Stereo td = {(input.left & d.inp_l) + (dry.left & d.snd_l) + (ext.left & d.ext_l),
                       (input.right & d.inp_r) + (dry.right & d.snd_r) + (ext.right & d.ext_r)};
    const CoreGates& w = c.wet_gate;
    const Stereo tw = {(input.left & w.inp_l) + (wet.left & w.snd_l) + (ext.left & w.ext_l),
                       (input.right & w.inp_r) + (wet.right & w.snd_r) + (ext.right & w.ext_r)};
    const Stereo rv = do_reverb(c, tw);
    return add(td, volume(rv, c.fx_vol.left, c.fx_vol.right));
}

Stereo Chip::mix() {
    ++cycles_;
    for (Core& c : cores_) {
        if (c.key_off != 0) {
            for (std::uint32_t v = 0; v < voices_per_core; ++v) {
                if (((c.key_off >> v) & 1) != 0) {
                    c.voices[v].env.start_release();
                }
            }
            c.key_off = 0;
        }
        if (c.key_on != 0) {
            c.endx &= ~c.key_on;
            for (std::uint32_t v = 0; v < voices_per_core; ++v) {
                if (((c.key_on >> v) & 1) != 0) {
                    c.voices[v].key_on();
                }
            }
            c.key_on = 0;
        }
    }

    Core& c0 = cores_[0];
    Core& c1 = cores_[1];
    const Stereo in0 = volume(read_input(c0), c0.inp_vol.left, c0.inp_vol.right);
    const Stereo in1 = volume(read_input(c1), c1.inp_vol.left, c1.inp_vol.right);

    std::array<Stereo, 2> dry{};
    std::array<Stereo, 2> wet{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        Core& c = cores_[i];
        for (std::uint32_t v = 0; v < voices_per_core; ++v) {
            const Stereo s = mix_voice(c, v);
            const VoiceGates& g = c.voice_gates[v];
            dry[i].left += s.left & g.dry_l;
            dry[i].right += s.right & g.dry_r;
            wet[i].left += s.left & g.wet_l;
            wet[i].right += s.right & g.wet_r;
        }
    }

    Stereo ext = mix_core(c0, dry[0], wet[0], in0, {});
    if ((play_mode_ & 4) != 0) {
        ext = {};
    } else {
        ext = volume(clamp16(ext), c0.master_left.value, c0.master_right.value);
    }
    // Core 0's output goes into sound RAM, then into core 1's external input.
    write_fast(0x800u + out_pos_, ext.left);
    write_fast(0xA00u + out_pos_, ext.right);
    ext = volume(ext, c1.ext_vol.left, c1.ext_vol.right);
    Stereo out = mix_core(c1, dry[1], wet[1], in1, ext);
    out = volume(clamp16(out), c1.master_left.value, c1.master_right.value);

    out_pos_ = static_cast<std::uint16_t>((out_pos_ + 1) & 0x1FF);
    return clamp16(out);
}

// ---------------------------------------------------------------------------------------------------------
// Signal prediction

std::uint32_t Chip::samples_to_signal(std::uint32_t limit, bool predict_voices) const {
    std::uint32_t k = std::max<std::uint32_t>(1, limit);
    // AutoDMA: the transfer ends in the sample whose MADR walk reaches the end after the last refill.
    for (const Core& c : cores_) {
        if (c.input_transferred == 0 && c.input_left == 0) {
            continue;
        }
        if (c.input_left == 0) {
            k = std::min(k, (c.input_transferred + 0x17F) / 0x180);
        } else {
            const std::uint32_t period = play_mode_ == 2 && c.index == 0 ? 0x40u : 0x80u;
            const std::uint32_t to_refill = (period - (out_pos_ & (period - 1))) & (period - 1);
            k = std::min(k, to_refill + 2);
        }
    }
    // IRQs: only cores whose IRQ is enabled and not yet raised can signal.
    std::array<std::uint32_t, 2> armed{};
    std::size_t n_armed = 0;
    for (std::uint32_t i = 0; i < 2; ++i) {
        if (cores_[i].irq_enable && (spdif_.info & (4u << i)) == 0) {
            armed[n_armed++] = cores_[i].irqa;
        }
    }
    const auto hits = [&](std::uint32_t address) {
        for (std::size_t i = 0; i < n_armed; ++i) {
            if (armed[i] == address) {
                return true;
            }
        }
        return false;
    };
    for (std::size_t i = 0; i < n_armed && k > 1; ++i) {
        const std::uint32_t a = armed[i];
        // The output areas (0x400-0x1FFF) and the input areas are written and read at OutPos.
        const std::uint32_t masked = a & 0xFFFFFDFFu;
        const bool input = (masked >= 0x2000 && masked < 0x2200) || (masked >= 0x2400 && masked < 0x2600);
        if ((a >= 0x400 && a < 0x2000) || input) {
            k = std::min(k, ((a - out_pos_) & 0x1FF) + 1);
        }
        for (const Core& c : cores_) {
            if (c.fx_enable && c.effects_start < c.effects_end && a >= c.effects_start &&
                a <= c.effects_end) {
                k = 1;
            }
        }
    }
    if (n_armed == 0 || k <= 1) {
        return k;
    }
    // A pending KON moves its voices at the next sample: predict once it has been applied.
    if (!predict_voices || (cores_[0].key_on | cores_[1].key_on) != 0) {
        return 1;
    }
    // Voices: walk each voice's addresses as if it advanced one word every sample (never slower than it can).
    for (const Core& c : cores_) {
        for (const Voice& vc : c.voices) {
            std::uint32_t address = vc.next;
            std::uint32_t loop_start = vc.loop_start;
            for (std::uint32_t d = 0; d + 1 < k; ++d) {
                const std::uint32_t header = address & 0xFFFF8u;
                if (hits(header)) {
                    k = d + 1;
                    break;
                }
                const auto flags = static_cast<std::int8_t>(static_cast<std::int16_t>(ram_[header]) >> 8);
                if ((flags & flag_loop_start) != 0 && vc.loop_mode == 0) {
                    loop_start = header;
                }
                if (hits(address)) {
                    k = d + 1;
                    break;
                }
                address = (address + 1) & address_mask;
                if ((address & 7) == 0) {
                    if ((flags & flag_loop_end) != 0) {
                        address = loop_start;
                    }
                    if (hits(address)) {
                        k = d + 1;
                        break;
                    }
                    address = (address + 1) & address_mask;
                }
            }
        }
    }
    return k;
}

} // namespace dq8::rt::spu2
