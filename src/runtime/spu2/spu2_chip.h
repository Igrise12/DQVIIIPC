// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Adapted from PCSX2 (PCSX2/pcsx2, 3e29183a37e7, tag v2.7.524), PCSX2 Dev Team: the state of
// pcsx2/SPU2/defs.h (V_Core, V_Voice, V_ADSR, V_VolumeSlide, V_Reverb, V_SPDIF).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// The SPU2 as a pure state machine (architecture §7.1, ADR-0014): two cores of 24 voices, 2 MiB of sound RAM
// in 16-bit words, the register file at IOP 0x1F900000, ADPCM decoding, envelopes, volume slides, noise,
// pitch modulation, the dry/wet routing, core 0 into core 1, reverb, and the data side of manual DMA and
// AutoDMA. It knows nothing of time or of the IOP: the device (spu2.h) calls mix() once per 48 kHz sample at
// the right virtual time, moves DMA data in and out at the right times, and turns the signals mix() leaves
// (an IRQ raised, an AutoDMA transfer finished) into IOP interrupts and DMA completions. Behaviour follows
// PCSX2's SPU2 (pcsx2/SPU2/, tag v2.7.524), integer arithmetic only.
namespace dq8::rt::spu2 {

inline constexpr std::uint32_t ram_words = 0x100000; // 2 MiB of 16-bit words
inline constexpr std::uint32_t address_mask = 0xFFFFF;
inline constexpr std::uint32_t voices_per_core = 24;
inline constexpr std::uint32_t register_bytes = 0x800;

// Thrown by the chip for register values that select hardware modes we do not model (CDDA, PS1 mode); the
// device turns it into a machine stop.
class Unsupported : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

struct Stereo {
    std::int32_t left = 0;
    std::int32_t right = 0;
};

// A voice or master volume: the register value and, in sweep mode, the current level.
struct VolumeSlide {
    std::uint16_t reg = 0;
    std::uint32_t counter = 0;
    std::int32_t value = 0;

    void set(std::uint16_t v);
    void update();
};

struct Envelope {
    static constexpr std::uint8_t stopped = 0;
    static constexpr std::uint8_t attack = 1;
    static constexpr std::uint8_t decay = 2;
    static constexpr std::uint8_t sustain = 3;
    static constexpr std::uint8_t release = 4;

    struct Phase {
        bool decr = false;
        bool exp = false;
        std::uint8_t shift = 0;
        std::int8_t step = 0;
        std::int32_t target = 0;
    };

    std::uint16_t adsr1 = 0;
    std::uint16_t adsr2 = 0;
    std::array<Phase, 5> phases{};
    std::uint32_t counter = 0;
    std::int32_t value = 0; // ENVX, 0..0x7FFF
    std::uint8_t phase = stopped;

    void update_cache();
    // One sample; false when the envelope ended (the voice stops).
    bool step();
    void start_attack();
    void start_release();
};

struct Voice {
    VolumeSlide vol_left;
    VolumeSlide vol_right;
    Envelope env;
    std::uint16_t pitch = 0;
    std::uint32_t loop_start = 0; // LSAX
    std::uint32_t start = 0;      // SSA
    std::uint32_t next = 0;       // NAX
    std::int32_t prev1 = 0;
    std::int32_t prev2 = 0;
    bool modulated = false; // PMON
    bool noise = false;     // NON
    std::int8_t loop_mode = 0;
    std::int8_t loop_flags = 0;
    std::int32_t sp = 0; // pitch counter, 12-bit fraction
    std::int32_t out_x = 0;
    // The decoded ADPCM block the voice reads (PCSX2's SBuffer, one per voice instead of a shared cache).
    bool block_valid = false;
    std::array<std::int16_t, 28> block{};
    std::array<std::int32_t, 32> fifo{};
    std::uint32_t fifo_write = 0;
    std::uint32_t fifo_read = 0;

    void key_on();
    void stop();
};

struct VoiceGates {
    std::int32_t dry_l = -1;
    std::int32_t dry_r = -1;
    std::int32_t wet_l = -1;
    std::int32_t wet_r = -1;
};

struct CoreGates {
    std::int32_t inp_l = -1;
    std::int32_t inp_r = -1;
    std::int32_t snd_l = -1;
    std::int32_t snd_r = -1;
    std::int32_t ext_l = -1;
    std::int32_t ext_r = -1;
};

// The reverb registers. The addresses are offsets into the work area in 16-bit words.
struct Reverb {
    std::int16_t in_coef_l = 0;
    std::int16_t in_coef_r = 0;
    std::uint32_t apf1_size = 0;
    std::uint32_t apf2_size = 0;
    std::int16_t apf1_vol = 0;
    std::int16_t apf2_vol = 0;
    std::uint32_t same_l_src = 0;
    std::uint32_t same_r_src = 0;
    std::uint32_t diff_l_src = 0;
    std::uint32_t diff_r_src = 0;
    std::uint32_t same_l_dst = 0;
    std::uint32_t same_r_dst = 0;
    std::uint32_t diff_l_dst = 0;
    std::uint32_t diff_r_dst = 0;
    std::int16_t iir_vol = 0;
    std::int16_t wall_vol = 0;
    std::uint32_t comb1_l_src = 0;
    std::uint32_t comb1_r_src = 0;
    std::uint32_t comb2_l_src = 0;
    std::uint32_t comb2_r_src = 0;
    std::uint32_t comb3_l_src = 0;
    std::uint32_t comb3_r_src = 0;
    std::uint32_t comb4_l_src = 0;
    std::uint32_t comb4_r_src = 0;
    std::int16_t comb1_vol = 0;
    std::int16_t comb2_vol = 0;
    std::int16_t comb3_vol = 0;
    std::int16_t comb4_vol = 0;
    std::uint32_t apf1_l_dst = 0;
    std::uint32_t apf1_r_dst = 0;
    std::uint32_t apf2_l_dst = 0;
    std::uint32_t apf2_r_dst = 0;
};

struct Core {
    std::uint32_t index = 0;
    std::array<VoiceGates, voices_per_core> voice_gates{};
    CoreGates dry_gate;
    CoreGates wet_gate;
    VolumeSlide master_left;
    VolumeSlide master_right;
    Stereo ext_vol;
    Stereo inp_vol;
    Stereo fx_vol;
    std::array<Voice, voices_per_core> voices{};

    std::uint32_t irqa = 0;
    std::uint32_t tsa = 0;
    std::uint32_t active_tsa = 0;
    bool irq_enable = false;
    bool fx_enable = false;
    bool adma_in_progress = false;
    std::uint8_t dma_bits = 0;
    std::uint8_t noise_clock = 0;
    std::uint32_t noise_count = 0;
    std::uint32_t noise_out = 0;
    std::uint16_t adma_ctrl = 0; // ADMAS

    // AutoDMA input (PCSX2's InputDataLeft, InputDataTransferred, InputPosWrite, InputDataProgress), in
    // 16-bit words, and where the running transfer reads IOP RAM.
    std::uint32_t input_left = 0;
    std::uint32_t input_transferred = 0;
    std::uint32_t input_pos_write = 0;
    std::uint32_t input_progress = 0;
    std::uint32_t adma_madr = 0;

    Reverb revb;
    std::array<std::array<std::int16_t, 128>, 2> revb_down{};
    std::array<std::array<std::int16_t, 128>, 2> revb_up{};
    std::uint32_t revb_pos = 0;
    std::uint32_t effects_start = 0; // ESA
    std::uint32_t effects_end = 0;   // EEA

    std::uint32_t pmon = 0;
    std::uint32_t non = 0;
    std::uint32_t vmixl = 0;
    std::uint32_t vmixel = 0;
    std::uint32_t vmixr = 0;
    std::uint32_t vmixer = 0;
    std::uint32_t endx = 0;
    std::uint16_t mmix = 0;
    std::uint16_t statx = 0;
    std::uint16_t attr = 0;
    std::uint8_t attr_bit0 = 0;
    std::uint8_t dma_mode = 0;
    std::uint32_t key_on = 0; // latched KON and KOFF, applied before the next sample
    std::uint32_t key_off = 0;
};

struct Spdif {
    std::uint16_t out = 0;
    std::uint16_t info = 0; // the IRQ status: bit 2 core 0, bit 3 core 1
    std::uint16_t unknown1 = 0;
    std::uint16_t mode = 0;
    std::uint16_t media = 0;
    std::uint16_t unknown2 = 0;
    std::uint16_t protection = 0;
};

// Decodes one 16-byte ADPCM block (the header word, then 28 four-bit samples in seven words) and carries
// the decoder's two-sample history.
void decode_adpcm_block(std::span<const std::uint16_t, 8> block, std::array<std::int16_t, 28>& out,
                        std::int32_t& prev1, std::int32_t& prev2);

// How a DMA start proceeds. Times are in DMA word units (one 16-bit word: 24 IOP cycles in PCSX2,
// CostModel::spu2_dma_per_halfword EE cycles here).
struct DmaPlan {
    bool deferred = false;        // AutoDMA: the chip signals the end through take_adma_done()
    std::uint32_t first_step = 0; // units from the start to the first dma_step()
    std::uint32_t total = 0;      // units from the start to the completion (not deferred)
};

class Chip {
  public:
    Chip();

    // Power-on state (PCSX2's InternalReset and V_Core::Init).
    void reset();

    // Register access, offset 0..0x7FE from 0x1F900000, 16 bits.
    std::uint16_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint16_t value);

    // One 48 kHz sample: KOFF and KON latched since the last sample, then the mix. Returns core 1's output
    // after its master volume, clamped to 16 bits.
    Stereo mix();

    // Signals since the last take: bit i for core i. An IRQ raised (the SPDIF IRQ status bit went from 0 to
    // 1), and an AutoDMA transfer that finished (its channel completes).
    std::uint32_t take_irqs() noexcept;
    std::uint32_t take_adma_done() noexcept;

    // IOP RAM for AutoDMA and DMA (2 MiB, little-endian words). Null until the device connects.
    void set_iop_ram(std::uint8_t* ram, std::size_t size) noexcept {
        iop_ram_ = ram;
        iop_ram_size_ = size;
    }

    // A DMA start on core `core`'s channel (PCSX2's DoDMAwrite, DoDMAread, StartADMAWrite). Manual
    // transfers move their data in chunks: a write copies a chunk at its start, a read at its end, and each
    // chunk's IRQA hits are raised when the chunk's time is up, as in PCSX2's FinishDMAwrite/FinishDMAread
    // and CheckDMAProgress. The device calls dma_step() at every step time; it returns the units to the next
    // step, or 0 when the transfer is done (the chip has applied PCSX2's SPU2interruptDMA4/7 and the channel
    // completes). An AutoDMA start is deferred: its data is pulled at the sample rate.
    DmaPlan begin_dma(std::uint32_t core, bool read, std::uint32_t madr, std::uint32_t words);
    std::uint32_t dma_step(std::uint32_t core);
    // The guest cleared the channel's busy bit: abandon the rest of a manual transfer, or stop pulling
    // AutoDMA input.
    void stop_dma(std::uint32_t core);
    bool dma_active(std::uint32_t core) const noexcept { return dma_[core].active; }
    // Whether a DMA write on this core feeds the AutoDMA input (ADMAS has the core's bit).
    bool adma_enabled(std::uint32_t core) const noexcept;

    // A lower bound k, 1 <= k <= limit, on the samples until one may leave a signal: mixing k - 1 samples
    // leaves none. Exact for AutoDMA completions and the input and output areas; conservative for voices (as
    // if each advanced one word every sample) and for reverb (1). Without `predict_voices`, 1 whenever an IRQ
    // is armed.
    std::uint32_t samples_to_signal(std::uint32_t limit, bool predict_voices = true) const;

    std::span<const std::uint16_t> ram() const noexcept { return ram_; }
    std::span<std::uint16_t> ram() noexcept { return ram_; }
    const Core& core(std::uint32_t i) const noexcept { return cores_[i]; }
    const Spdif& spdif() const noexcept { return spdif_; }
    std::uint32_t sample_count() const noexcept { return cycles_; }
    std::uint16_t out_pos() const noexcept { return out_pos_; }
    // Every field that shapes future output, serialised little-endian: for determinism checks.
    std::vector<std::uint8_t> serialize_state() const;

  private:
    struct DmaState {
        bool active = false;
        bool read = false;
        std::uint32_t madr = 0;      // IOP address of the next word
        std::uint32_t remaining = 0; // words not yet moved
    };

    std::uint32_t dma_write_chunk(Core& c);
    std::uint32_t dma_read_chunk(Core& c);
    void finish_dma(Core& c);
    void deliver_dma_irqs();

    // Registers.
    void write_core(Core& c, std::uint32_t o, std::uint16_t value);
    std::uint16_t read_core(const Core& c, std::uint32_t o) const;
    void write_ext(Core& c, std::uint32_t o, std::uint16_t value);
    std::uint16_t read_ext(const Core& c, std::uint32_t o) const;
    void write_attr(Core& c, std::uint16_t value);
    void write_admas(Core& c, std::uint16_t value);
    void update_spdif_mode();
    std::uint16_t& raw(std::uint32_t offset) { return raw_[offset >> 1]; }

    // Mixing (spu2_mix.cpp).
    void set_irq(std::uint32_t core);
    void check_irq(std::uint32_t address);
    void write_fast(std::uint32_t address, std::int32_t value);
    void decode_samples(Core& c, std::uint32_t v);
    void increment_next(Voice& vc);
    Stereo mix_voice(Core& c, std::uint32_t v);
    Stereo mix_core(Core& c, Stereo dry, Stereo wet, Stereo input, Stereo ext);
    Stereo read_input(Core& c);
    void adma_read_buffer(Core& c);
    Stereo do_reverb(Core& c, Stereo input);
    std::uint32_t reverb_index(const Core& c, std::int32_t offset) const;
    void update_noise(Core& c);
    std::uint16_t iop_word(std::uint32_t address) const;
    void set_iop_word(std::uint32_t address, std::uint16_t value);

    std::vector<std::uint16_t> ram_;
    std::array<std::uint16_t, register_bytes / 2> raw_{};
    std::array<Core, 2> cores_{};
    Spdif spdif_;
    std::uint16_t out_pos_ = 0;
    std::uint32_t cycles_ =
        0; // samples mixed (PCSX2's Cycles; reverb runs left and right on alternate samples)
    int play_mode_ = 0;
    std::array<bool, 2> irq_dma_pending_{false, false};
    std::array<DmaState, 2> dma_{};
    std::uint32_t irq_signals_ = 0;
    std::uint32_t adma_done_ = 0;
    std::uint8_t* iop_ram_ = nullptr;
    std::size_t iop_ram_size_ = 0;
};

} // namespace dq8::rt::spu2
