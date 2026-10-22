// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Adapted from PCSX2 (PCSX2/pcsx2, 3e29183a37e7, tag v2.7.524), PCSX2 Dev Team: pcsx2/SPU2/spu2sys.cpp
// (V_Core::Init, the register write handlers, StartVoices, StopVoices, UpdateSpdifMode), RegTable.cpp (what
// a register read returns), spu2.cpp (InternalReset, SPU2interruptDMA4/7) and Dma.cpp (DoDMAwrite,
// DoDMAread, FinishDMAwrite, FinishDMAread, StartADMAWrite), with CheckDMAProgress's chunk timing.
#include "runtime/spu2/spu2_chip.h"

#include <algorithm>
#include <string>

namespace dq8::rt::spu2 {

namespace {

constexpr std::uint16_t lo16(std::uint32_t v) {
    return static_cast<std::uint16_t>(v);
}
constexpr std::uint16_t hi16(std::uint32_t v) {
    return static_cast<std::uint16_t>(v >> 16);
}
constexpr std::uint32_t with_lo(std::uint32_t v, std::uint16_t lo) {
    return (v & 0xFFFF0000u) | lo;
}
constexpr std::uint32_t with_hi(std::uint32_t v, std::uint16_t hi) {
    return (v & 0xFFFFu) | (std::uint32_t{hi} << 16);
}

// The 22 address registers of the reverb, in register order from core offset 0x2E4 (two halves each).
std::uint32_t* reverb_address(Reverb& r, std::uint32_t index) {
    std::uint32_t* const regs[] = {
        &r.apf1_size,   &r.apf2_size,   &r.same_l_dst,  &r.same_r_dst,  &r.comb1_l_src, &r.comb1_r_src,
        &r.comb2_l_src, &r.comb2_r_src, &r.same_l_src,  &r.same_r_src,  &r.diff_l_dst,  &r.diff_r_dst,
        &r.comb3_l_src, &r.comb3_r_src, &r.comb4_l_src, &r.comb4_r_src, &r.diff_l_src,  &r.diff_r_src,
        &r.apf1_l_dst,  &r.apf1_r_dst,  &r.apf2_l_dst,  &r.apf2_r_dst};
    return regs[index];
}
std::uint32_t reverb_address(const Reverb& r, std::uint32_t index) {
    return *reverb_address(const_cast<Reverb&>(r), index);
}

// The volume and coefficient registers of the 0x760 area, from index 10 (IIR_VOL) on.
std::int16_t* reverb_volume(Reverb& r, std::uint32_t index) {
    std::int16_t* const regs[] = {&r.iir_vol,  &r.comb1_vol, &r.comb2_vol, &r.comb3_vol, &r.comb4_vol,
                                  &r.wall_vol, &r.apf1_vol,  &r.apf2_vol,  &r.in_coef_l, &r.in_coef_r};
    return regs[index - 10];
}

void set_gates(Core& c, std::int32_t VoiceGates::* gate, std::uint32_t reg) {
    for (std::uint32_t v = 0; v < voices_per_core; ++v) {
        c.voice_gates[v].*gate = (reg >> v) & 1 ? -1 : 0;
    }
}

class Writer {
  public:
    explicit Writer(std::vector<std::uint8_t>& out) : out_(out) {}
    void u8(std::uint32_t v) { out_.push_back(static_cast<std::uint8_t>(v)); }
    void u16(std::uint32_t v) {
        u8(v);
        u8(v >> 8);
    }
    void u32(std::uint32_t v) {
        u16(v);
        u16(v >> 16);
    }
    void s32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void slide(const VolumeSlide& s) {
        u16(s.reg);
        u32(s.counter);
        s32(s.value);
    }

  private:
    std::vector<std::uint8_t>& out_;
};

} // namespace

Chip::Chip() : ram_(ram_words, 0) {
    reset();
}

void Chip::reset() {
    std::fill(ram_.begin(), ram_.end(), std::uint16_t{0});
    // From PCSX2's InternalReset ("from BIOS reversal"): two blocks of 0x07 bytes, the first one parking the
    // voices on a silent loop at 0x2800.
    std::fill_n(ram_.begin() + 0x2800, 8, std::uint16_t{0x0707});
    std::fill_n(ram_.begin() + 0xE870, 8, std::uint16_t{0x0707});
    raw_.fill(0);
    spdif_ = Spdif{};
    out_pos_ = 0;
    cycles_ = 0;
    play_mode_ = 0;
    irq_dma_pending_ = {false, false};
    dma_ = {};
    irq_signals_ = 0;
    adma_done_ = 0;

    for (std::uint32_t i = 0; i < 2; ++i) {
        Core& c = cores_[i];
        c = Core{};
        c.index = i;
        c.input_pos_write = 0x100;
        c.statx = 0x80;
        c.ext_vol = {0x7FFF, 0x7FFF};
        c.inp_vol = {0x7FFF, 0x7FFF};
        c.fx_vol = {0, 0};
        c.dry_gate = CoreGates{};
        c.wet_gate = CoreGates{};
        c.dry_gate.ext_l = 0;
        c.dry_gate.ext_r = 0;
        if (i == 0) {
            c.wet_gate.ext_l = 0;
            c.wet_gate.ext_r = 0;
        }
        c.mmix = i != 0 ? 0xFFC : 0xFF0;
        c.vmixl = c.vmixr = c.vmixel = c.vmixer = 0xFFFFFF;
        c.effects_start = i != 0 ? 0xFFFF8 : 0xEFFF8;
        c.effects_end = i != 0 ? 0xFFFFF : 0xEFFFF;
        // PCSX2: "the exact boot value is unknown [...] somewhere in the input or output areas".
        c.irqa = 0x800;
        c.endx = 0xFFFFFF;
        for (Voice& vc : c.voices) {
            vc.pitch = 0x3FFF;
            vc.next = 0x2801;
            vc.start = 0x2800;
            vc.loop_start = 0x2800;
        }
    }
}

std::uint32_t Chip::take_irqs() noexcept {
    const std::uint32_t s = irq_signals_;
    irq_signals_ = 0;
    return s;
}

std::uint32_t Chip::take_adma_done() noexcept {
    const std::uint32_t s = adma_done_;
    adma_done_ = 0;
    return s;
}

// ---------------------------------------------------------------------------------------------------------
// Registers

std::uint16_t Chip::read(std::uint32_t offset) const {
    offset &= register_bytes - 2;
    if (offset < 0x760) {
        return read_core(cores_[offset >> 10], offset & 0x3FF);
    }
    if (offset < 0x7B0) {
        const std::uint32_t o = offset - 0x760;
        return read_ext(cores_[o / 0x28], o % 0x28);
    }
    switch (offset) {
    case 0x7C0:
        return spdif_.out;
    case 0x7C2:
        return spdif_.info;
    case 0x7C4:
        return spdif_.unknown1;
    case 0x7C6:
        return spdif_.mode;
    case 0x7C8:
        return spdif_.media;
    case 0x7CA:
        return spdif_.unknown2;
    case 0x7CC:
        return spdif_.protection;
    default:
        return raw_[offset >> 1];
    }
}

void Chip::write(std::uint32_t offset, std::uint16_t value) {
    offset &= register_bytes - 2;
    if (offset < 0x760) {
        write_core(cores_[offset >> 10], offset & 0x3FF, value);
        return;
    }
    if (offset < 0x7B0) {
        const std::uint32_t o = offset - 0x760;
        write_ext(cores_[o / 0x28], o % 0x28, value);
        return;
    }
    switch (offset) {
    case 0x7C0:
        spdif_.out = value;
        update_spdif_mode();
        return;
    case 0x7C2:
        spdif_.info = value;
        update_spdif_mode();
        return;
    case 0x7C4:
        spdif_.unknown1 = value;
        return;
    case 0x7C6:
        spdif_.mode = value;
        update_spdif_mode();
        return;
    case 0x7C8:
        spdif_.media = value;
        update_spdif_mode();
        return;
    case 0x7CA:
        spdif_.unknown2 = value;
        return;
    case 0x7CC:
        spdif_.protection = value;
        update_spdif_mode();
        return;
    default:
        raw_[offset >> 1] = value;
        return;
    }
}

std::uint16_t Chip::read_core(const Core& c, std::uint32_t o) const {
    if (o < 0x180) {
        const Voice& vc = c.voices[o >> 4];
        switch ((o >> 1) & 7) {
        case 0:
            return vc.vol_left.reg;
        case 1:
            return vc.vol_right.reg;
        case 2:
            return vc.pitch;
        case 3:
            return vc.env.adsr1;
        case 4:
            return vc.env.adsr2;
        case 5:
            return lo16(static_cast<std::uint32_t>(vc.env.value));
        case 6:
            return lo16(static_cast<std::uint32_t>(vc.vol_left.value));
        default:
            return lo16(static_cast<std::uint32_t>(vc.vol_right.value));
        }
    }
    if (o >= 0x1C0 && o < 0x2E0) {
        const Voice& vc = c.voices[(o - 0x1C0) / 12];
        switch (((o - 0x1C0) % 12) / 2) {
        case 0:
            return hi16(vc.start);
        case 1:
            return lo16(vc.start);
        case 2:
            return hi16(vc.loop_start);
        case 3:
            return lo16(vc.loop_start);
        case 4:
            return hi16(vc.next);
        default:
            return lo16(vc.next);
        }
    }
    if (o >= 0x2E4 && o < 0x33C) {
        const std::uint32_t v = reverb_address(c.revb, (o - 0x2E4) / 4);
        return (o & 2) == 0 ? hi16(v) : lo16(v);
    }
    const std::uint32_t full = o | (c.index << 10);
    switch (o) {
    case 0x180:
        return lo16(c.pmon);
    case 0x182:
        return hi16(c.pmon);
    case 0x184:
        return lo16(c.non);
    case 0x186:
        return hi16(c.non);
    case 0x188:
        return lo16(c.vmixl);
    case 0x18A:
        return hi16(c.vmixl);
    case 0x18C:
        return lo16(c.vmixel);
    case 0x18E:
        return hi16(c.vmixel);
    case 0x190:
        return lo16(c.vmixr);
    case 0x192:
        return hi16(c.vmixr);
    case 0x194:
        return lo16(c.vmixer);
    case 0x196:
        return hi16(c.vmixer);
    case 0x198:
        return c.mmix;
    case 0x19A:
        return c.attr;
    case 0x19C:
        return hi16(c.irqa);
    case 0x19E:
        return lo16(c.irqa);
    case 0x1A0:
        return lo16(c.key_on);
    case 0x1A2:
        return hi16(c.key_on);
    case 0x1A4:
        return lo16(c.key_off);
    case 0x1A6:
        return hi16(c.key_off);
    case 0x1A8:
        return hi16(c.tsa);
    case 0x1AA:
        return lo16(c.tsa);
    case 0x1B0:
        return c.adma_ctrl;
    case 0x2E0:
        return hi16(c.effects_start);
    case 0x2E2:
        return lo16(c.effects_start);
    case 0x33C:
        return hi16(c.effects_end);
    case 0x33E:
        return lo16(c.effects_end);
    case 0x340:
        return lo16(c.endx);
    case 0x342:
        return hi16(c.endx);
    case 0x344:
        return c.statx;
    default:
        return raw_[full >> 1];
    }
}

void Chip::write_core(Core& c, std::uint32_t o, std::uint16_t value) {
    if (o < 0x180) {
        Voice& vc = c.voices[o >> 4];
        switch ((o >> 1) & 7) {
        case 0:
            vc.vol_left.set(value);
            break;
        case 1:
            vc.vol_right.set(value);
            break;
        case 2:
            vc.pitch = value;
            break;
        case 3:
            vc.env.adsr1 = value;
            vc.env.update_cache();
            break;
        case 4:
            vc.env.adsr2 = value;
            vc.env.update_cache();
            break;
        case 5:
            vc.env.value = value; // ENVX is writable; VOLXL and VOLXR are not
            break;
        default:
            break;
        }
        return;
    }
    if (o >= 0x1C0 && o < 0x2E0) {
        Voice& vc = c.voices[(o - 0x1C0) / 12];
        const std::uint32_t hi = std::uint32_t{value & 0x0Fu} << 16;
        const std::uint32_t lo = value & 0xFFF8u;
        switch (((o - 0x1C0) % 12) / 2) {
        case 0:
            vc.start = hi | (vc.start & 0xFFF8u);
            break;
        case 1:
            vc.start = (vc.start & 0xF0000u) | lo;
            break;
        case 2:
            vc.loop_mode = 1;
            vc.loop_start = hi | (vc.loop_start & 0xFFF8u);
            break;
        case 3:
            vc.loop_mode = 1;
            vc.loop_start = (vc.loop_start & 0xF0000u) | lo;
            break;
        case 4: // NAX is writable: the decoder continues from there
            vc.next = hi | (vc.next & 0xFFF8u) | 1;
            break;
        default:
            vc.next = (vc.next & 0xF0000u) | lo | 1;
            break;
        }
        return;
    }
    if (o >= 0x2E4 && o < 0x33C) {
        std::uint32_t index = (o - 0x2E4) / 4;
        // Core 1's DIFF_L_SRC and DIFF_R_SRC trade places on writes (PCSX2's write table), not on reads.
        if (c.index == 1 && (index == 16 || index == 17)) {
            index ^= 1;
        }
        std::uint32_t& reg = *reverb_address(c.revb, index);
        reg = (o & 2) == 0 ? with_hi(reg, value) : with_lo(reg, value);
        return;
    }
    const std::uint32_t full = o | (c.index << 10);
    switch (o) {
    case 0x180:
        for (std::uint32_t v = 1; v < 16; ++v) {
            c.voices[v].modulated = ((value >> v) & 1) != 0;
        }
        c.pmon = with_lo(c.pmon, value);
        break;
    case 0x182:
        for (std::uint32_t v = 0; v < 8; ++v) {
            c.voices[v + 16].modulated = ((value >> v) & 1) != 0;
        }
        c.pmon = with_hi(c.pmon, value);
        break;
    case 0x184:
        for (std::uint32_t v = 0; v < 16; ++v) {
            c.voices[v].noise = ((value >> v) & 1) != 0;
        }
        c.non = with_lo(c.non, value);
        break;
    case 0x186:
        for (std::uint32_t v = 0; v < 8; ++v) {
            c.voices[v + 16].noise = ((value >> v) & 1) != 0;
        }
        c.non = with_hi(c.non, value);
        break;
    case 0x188:
    case 0x18A:
        c.vmixl = o == 0x188 ? with_lo(c.vmixl, value) : with_hi(c.vmixl, value);
        set_gates(c, &VoiceGates::dry_l, c.vmixl);
        break;
    case 0x18C:
    case 0x18E:
        c.vmixel = o == 0x18C ? with_lo(c.vmixel, value) : with_hi(c.vmixel, value);
        set_gates(c, &VoiceGates::wet_l, c.vmixel);
        break;
    case 0x190:
    case 0x192:
        c.vmixr = o == 0x190 ? with_lo(c.vmixr, value) : with_hi(c.vmixr, value);
        set_gates(c, &VoiceGates::dry_r, c.vmixr);
        break;
    case 0x194:
    case 0x196:
        c.vmixer = o == 0x194 ? with_lo(c.vmixer, value) : with_hi(c.vmixer, value);
        set_gates(c, &VoiceGates::wet_r, c.vmixer);
        break;
    case 0x198: {
        const std::uint32_t vx = value & (c.index == 0 ? 0xFF0u : 0xFFFu);
        const auto gate = [vx](std::uint32_t bit) { return (vx & bit) != 0 ? -1 : 0; };
        c.wet_gate.ext_r = gate(0x001);
        c.wet_gate.ext_l = gate(0x002);
        c.dry_gate.ext_r = gate(0x004);
        c.dry_gate.ext_l = gate(0x008);
        c.wet_gate.inp_r = gate(0x010);
        c.wet_gate.inp_l = gate(0x020);
        c.dry_gate.inp_r = gate(0x040);
        c.dry_gate.inp_l = gate(0x080);
        c.wet_gate.snd_r = gate(0x100);
        c.wet_gate.snd_l = gate(0x200);
        c.dry_gate.snd_r = gate(0x400);
        c.dry_gate.snd_l = gate(0x800);
        c.mmix = value;
        break;
    }
    case 0x19A:
        write_attr(c, value);
        break;
    case 0x19C:
        c.irqa = with_hi(c.irqa, value);
        break;
    case 0x19E:
        c.irqa = with_lo(c.irqa, value);
        break;
    case 0x1A0:
        c.key_on = with_lo(c.key_on, value);
        break;
    case 0x1A2:
        c.key_on = with_hi(c.key_on, value);
        break;
    case 0x1A4:
        c.key_off = with_lo(c.key_off, value);
        break;
    case 0x1A6:
        c.key_off = with_hi(c.key_off, value);
        break;
    case 0x1A8:
        c.tsa = with_hi(c.tsa, value);
        break;
    case 0x1AA:
        c.tsa = with_lo(c.tsa, value);
        break;
    case 0x1AC:
        // A transfer through the data register: one word at TSA, which advances.
        c.active_tsa = c.tsa;
        check_irq(c.active_tsa);
        ram_[c.active_tsa & address_mask] = value;
        c.active_tsa = (c.active_tsa + 1) & address_mask;
        c.tsa = c.active_tsa;
        break;
    case 0x1B0:
        write_admas(c, value);
        break;
    case 0x2E0:
        c.effects_start = with_hi(c.effects_start, value);
        break;
    case 0x2E2:
        c.effects_start = with_lo(c.effects_start, value);
        break;
    case 0x33C: // EEA sets the upper half only
        c.effects_end = with_hi(c.effects_end, value);
        break;
    case 0x33E:
        break;
    case 0x340: // ENDX: any write clears its half
        c.endx &= 0xFF0000u;
        break;
    case 0x342:
        c.endx &= 0xFFFFu;
        break;
    case 0x344:
        c.statx = value;
        break;
    default:
        raw_[full >> 1] = value;
        break;
    }
}

void Chip::write_attr(Core& c, std::uint16_t value) {
    const bool old_irq = c.irq_enable;
    const std::uint8_t old_dma_mode = c.dma_mode;
    c.attr_bit0 = value & 1;
    c.dma_bits = (value >> 1) & 7;
    c.dma_mode = (value >> 4) & 3;
    c.irq_enable = ((value >> 6) & 1) != 0;
    c.fx_enable = ((value >> 7) & 1) != 0;
    c.noise_clock = (value >> 8) & 0x3F;
    c.attr = value;
    if (c.dma_mode == 0 && (c.statx & 0x400) == 0) {
        c.statx &= ~0x80;
    } else if (old_dma_mode == 0 && c.dma_mode != 0) {
        c.statx |= 0x80;
    }
    c.active_tsa = c.tsa;
    // Disabling the IRQ acknowledges it: the next hit raises it again.
    if (old_irq != c.irq_enable && !c.irq_enable) {
        spdif_.info &= static_cast<std::uint16_t>(~(4u << c.index));
    }
}

void Chip::write_admas(Core& c, std::uint16_t value) {
    if (value == 0x7FFF) {
        throw Unsupported("ADMAS 0x7FFF selects PS1 mode, which is not modelled");
    }
    c.adma_ctrl = value;
    if ((value & 3) == 0 && c.adma_in_progress) {
        // AutoDMA switched off mid-stream: the transfer ends and the input buffers are cleared.
        c.adma_in_progress = false;
        c.input_left = 0;
        c.input_transferred = 0;
        std::fill_n(ram_.begin() + 0x2000 + (c.index << 10), 0x200, std::uint16_t{0});
        std::fill_n(ram_.begin() + 0x2200 + (c.index << 10), 0x200, std::uint16_t{0});
    }
}

std::uint16_t Chip::read_ext(const Core& c, std::uint32_t o) const {
    switch (o / 2) {
    case 0:
        return c.master_left.reg;
    case 1:
        return c.master_right.reg;
    case 2:
        return lo16(static_cast<std::uint32_t>(c.fx_vol.left));
    case 3:
        return lo16(static_cast<std::uint32_t>(c.fx_vol.right));
    case 4:
        return lo16(static_cast<std::uint32_t>(c.ext_vol.left));
    case 5:
        return lo16(static_cast<std::uint32_t>(c.ext_vol.right));
    case 6:
        return lo16(static_cast<std::uint32_t>(c.inp_vol.left));
    case 7:
        return lo16(static_cast<std::uint32_t>(c.inp_vol.right));
    case 8:
        return lo16(static_cast<std::uint32_t>(c.master_left.value));
    case 9:
        return lo16(static_cast<std::uint32_t>(c.master_right.value));
    default:
        return static_cast<std::uint16_t>(*reverb_volume(const_cast<Reverb&>(c.revb), o / 2));
    }
}

void Chip::write_ext(Core& c, std::uint32_t o, std::uint16_t value) {
    const auto s = static_cast<std::int32_t>(static_cast<std::int16_t>(value));
    switch (o / 2) {
    case 0:
        c.master_left.set(value);
        break;
    case 1:
        c.master_right.set(value);
        break;
    case 2:
        c.fx_vol.left = s;
        break;
    case 3:
        c.fx_vol.right = s;
        break;
    case 4:
        c.ext_vol.left = s;
        break;
    case 5:
        c.ext_vol.right = s;
        break;
    case 6:
        c.inp_vol.left = s;
        break;
    case 7:
        c.inp_vol.right = s;
        break;
    case 8: // MVOLXL and MVOLXR are read-only
    case 9:
        break;
    default:
        *reverb_volume(c.revb, o / 2) = static_cast<std::int16_t>(value);
        break;
    }
}

void Chip::update_spdif_mode() {
    if ((spdif_.out & 0x4) != 0) {
        throw Unsupported("SPDIF_OUT bit 2 selects CDDA (24/32-bit PCM) streaming, which is not modelled");
    }
    if ((spdif_.out & 0x100) != 0) { // bypass
        play_mode_ = (spdif_.mode & 0x2) != 0 ? 2 : 4;
    } else {
        play_mode_ = (spdif_.out & 0x20) != 0 ? 1 : 0;
    }
}

// ---------------------------------------------------------------------------------------------------------
// DMA

bool Chip::adma_enabled(std::uint32_t core) const noexcept {
    const std::uint32_t bit = core + 1;
    return (cores_[core].adma_ctrl & bit) == bit;
}

std::uint16_t Chip::iop_word(std::uint32_t address) const {
    if (iop_ram_ == nullptr) {
        return 0;
    }
    const std::size_t a = address & (iop_ram_size_ - 1) & ~std::size_t{1};
    return static_cast<std::uint16_t>(iop_ram_[a] | (iop_ram_[a + 1] << 8));
}

void Chip::set_iop_word(std::uint32_t address, std::uint16_t value) {
    if (iop_ram_ == nullptr) {
        return;
    }
    const std::size_t a = address & (iop_ram_size_ - 1) & ~std::size_t{1};
    iop_ram_[a] = static_cast<std::uint8_t>(value);
    iop_ram_[a + 1] = static_cast<std::uint8_t>(value >> 8);
}

DmaPlan Chip::begin_dma(std::uint32_t core, bool read, std::uint32_t madr, std::uint32_t words) {
    Core& c = cores_[core];
    DmaState& d = dma_[core];
    d = DmaState{};
    DmaPlan plan;
    if (read) {
        c.active_tsa = c.tsa & address_mask;
        c.statx &= ~0x80;
        c.statx |= 0x400;
        d = DmaState{true, true, madr, words};
        // Reads wait a chunk's time before moving each chunk, and one word's time after the last.
        plan.first_step = std::max<std::uint32_t>(1, std::min<std::uint32_t>(words, 0x100));
        plan.total = words + 1;
        return plan;
    }
    if (words < 2) {
        c.statx &= ~0x80;
        d = DmaState{true, false, madr, 0};
        plan.first_step = plan.total = 1;
        return plan;
    }
    c.active_tsa = c.tsa & address_mask;
    if (adma_enabled(core)) {
        c.input_progress = 0;
        if (words >= 256) {
            c.input_left = words;
            c.adma_madr = madr;
            if (c.input_pos_write != 0xFFFF) {
                adma_read_buffer(c);
            }
            c.adma_in_progress = true;
            plan.deferred = true;
            return plan;
        }
        // Too short for AutoDMA: nothing moves, and the channel completes after the transfer's time.
        c.input_left = 0;
        d = DmaState{true, false, madr, 0};
        plan.first_step = plan.total = words;
        return plan;
    }
    c.statx &= ~0x80;
    c.statx |= 0x400;
    d = DmaState{true, false, madr, words};
    plan.first_step = dma_write_chunk(c);
    plan.total = words;
    return plan;
}

std::uint32_t Chip::dma_step(std::uint32_t core) {
    Core& c = cores_[core];
    DmaState& d = dma_[core];
    if (!d.active) {
        return 0;
    }
    deliver_dma_irqs();
    if (d.remaining > 0) {
        if (!d.read) {
            return dma_write_chunk(c);
        }
        dma_read_chunk(c);
        return d.remaining > 0 ? std::min<std::uint32_t>(d.remaining, 0x100) : 1;
    }
    finish_dma(c);
    d.active = false;
    return 0;
}

std::uint32_t Chip::dma_write_chunk(Core& c) {
    DmaState& d = dma_[c.index];
    const std::uint32_t n = std::min<std::uint32_t>(d.remaining, 0x100 + d.remaining / 24);
    const std::uint32_t start = c.active_tsa;
    for (std::uint32_t i = 0; i < n; ++i) {
        ram_[(start + i) & address_mask] = iop_word(d.madr + 2 * i);
    }
    const std::uint32_t end = start + n;
    const bool wraps = end > ram_words;
    const std::uint32_t tda = wraps ? end - ram_words : end;
    // IRQA strictly inside the chunk (PCSX2: start and end are both exclusive), raised when its time is up.
    for (std::uint32_t i = 0; i < 2; ++i) {
        const std::uint32_t a = cores_[i].irqa;
        if (cores_[i].irq_enable && (wraps ? (a > start || a < tda) : (a > start && a < tda))) {
            irq_dma_pending_[i] = true;
        }
    }
    d.madr += 2 * n;
    d.remaining -= n;
    c.active_tsa = tda & address_mask;
    c.tsa = c.active_tsa;
    return n;
}

std::uint32_t Chip::dma_read_chunk(Core& c) {
    DmaState& d = dma_[c.index];
    const std::uint32_t n = std::min<std::uint32_t>(d.remaining, 0x100);
    const std::uint32_t start = c.active_tsa;
    for (std::uint32_t i = 0; i < n; ++i) {
        set_iop_word(d.madr + 2 * i, ram_[(start + i) & address_mask]);
    }
    const std::uint32_t end = start + n;
    const bool wraps = end > ram_words;
    const std::uint32_t tda = wraps ? end - ram_words : end;
    for (std::uint32_t i = 0; i < 2; ++i) {
        const std::uint32_t a = cores_[i].irqa;
        if (cores_[i].irq_enable && (wraps ? (a > start || a < tda) : (a > start && a < tda))) {
            irq_dma_pending_[i] = true;
        }
    }
    d.madr += 2 * n;
    d.remaining -= n;
    c.active_tsa = tda & address_mask;
    c.tsa = c.active_tsa;
    return n;
}

void Chip::deliver_dma_irqs() {
    for (std::uint32_t i = 0; i < 2; ++i) {
        if (irq_dma_pending_[i]) {
            irq_dma_pending_[i] = false;
            set_irq(i);
        }
    }
}

void Chip::finish_dma(Core& c) {
    if (c.dma_mode != 0) {
        c.statx |= 0x80;
    }
    c.statx &= ~0x400;
    c.tsa = c.active_tsa;
}

void Chip::stop_dma(std::uint32_t core) {
    Core& c = cores_[core];
    DmaState& d = dma_[core];
    if (d.active) {
        d = DmaState{};
        finish_dma(c);
    }
    c.input_left = 0;
    c.input_transferred = 0;
}

// ---------------------------------------------------------------------------------------------------------
// State

std::vector<std::uint8_t> Chip::serialize_state() const {
    std::vector<std::uint8_t> out;
    out.reserve(16'384);
    Writer w(out);
    for (const std::uint16_t r : raw_) {
        w.u16(r);
    }
    w.u16(spdif_.out);
    w.u16(spdif_.info);
    w.u16(spdif_.unknown1);
    w.u16(spdif_.mode);
    w.u16(spdif_.media);
    w.u16(spdif_.unknown2);
    w.u16(spdif_.protection);
    w.u16(out_pos_);
    w.u32(cycles_);
    w.u32(static_cast<std::uint32_t>(play_mode_));
    w.u8(irq_dma_pending_[0] ? 1 : 0);
    w.u8(irq_dma_pending_[1] ? 1 : 0);
    for (const DmaState& d : dma_) {
        w.u8(d.active ? 1 : 0);
        w.u8(d.read ? 1 : 0);
        w.u32(d.madr);
        w.u32(d.remaining);
    }
    for (const Core& c : cores_) {
        for (const VoiceGates& g : c.voice_gates) {
            w.s32(g.dry_l);
            w.s32(g.dry_r);
            w.s32(g.wet_l);
            w.s32(g.wet_r);
        }
        for (const CoreGates* g : {&c.dry_gate, &c.wet_gate}) {
            w.s32(g->inp_l);
            w.s32(g->inp_r);
            w.s32(g->snd_l);
            w.s32(g->snd_r);
            w.s32(g->ext_l);
            w.s32(g->ext_r);
        }
        w.slide(c.master_left);
        w.slide(c.master_right);
        for (const Stereo* s : {&c.ext_vol, &c.inp_vol, &c.fx_vol}) {
            w.s32(s->left);
            w.s32(s->right);
        }
        for (const Voice& vc : c.voices) {
            w.slide(vc.vol_left);
            w.slide(vc.vol_right);
            w.u16(vc.env.adsr1);
            w.u16(vc.env.adsr2);
            for (const Envelope::Phase& p : vc.env.phases) {
                w.u8(p.decr ? 1 : 0);
                w.u8(p.exp ? 1 : 0);
                w.u8(p.shift);
                w.u8(static_cast<std::uint8_t>(p.step));
                w.s32(p.target);
            }
            w.u32(vc.env.counter);
            w.s32(vc.env.value);
            w.u8(vc.env.phase);
            w.u16(vc.pitch);
            w.u32(vc.loop_start);
            w.u32(vc.start);
            w.u32(vc.next);
            w.s32(vc.prev1);
            w.s32(vc.prev2);
            w.u8(vc.modulated ? 1 : 0);
            w.u8(vc.noise ? 1 : 0);
            w.u8(static_cast<std::uint8_t>(vc.loop_mode));
            w.u8(static_cast<std::uint8_t>(vc.loop_flags));
            w.s32(vc.sp);
            w.s32(vc.out_x);
            w.u8(vc.block_valid ? 1 : 0);
            for (const std::int16_t s : vc.block) {
                w.u16(static_cast<std::uint16_t>(s));
            }
            for (const std::int32_t s : vc.fifo) {
                w.s32(s);
            }
            w.u32(vc.fifo_write);
            w.u32(vc.fifo_read);
        }
        w.u32(c.irqa);
        w.u32(c.tsa);
        w.u32(c.active_tsa);
        w.u8(c.irq_enable ? 1 : 0);
        w.u8(c.fx_enable ? 1 : 0);
        w.u8(c.adma_in_progress ? 1 : 0);
        w.u8(c.dma_bits);
        w.u8(c.noise_clock);
        w.u32(c.noise_count);
        w.u32(c.noise_out);
        w.u16(c.adma_ctrl);
        w.u32(c.input_left);
        w.u32(c.input_transferred);
        w.u32(c.input_pos_write);
        w.u32(c.input_progress);
        w.u32(c.adma_madr);
        for (std::uint32_t i = 0; i < 22; ++i) {
            w.u32(reverb_address(c.revb, i));
        }
        for (std::uint32_t i = 10; i < 20; ++i) {
            w.u16(static_cast<std::uint16_t>(*reverb_volume(const_cast<Reverb&>(c.revb), i)));
        }
        for (const auto& buffers : {&c.revb_down, &c.revb_up}) {
            for (const auto& b : *buffers) {
                for (const std::int16_t s : b) {
                    w.u16(static_cast<std::uint16_t>(s));
                }
            }
        }
        w.u32(c.revb_pos);
        w.u32(c.effects_start);
        w.u32(c.effects_end);
        for (const std::uint32_t r : {c.pmon, c.non, c.vmixl, c.vmixel, c.vmixr, c.vmixer, c.endx}) {
            w.u32(r);
        }
        w.u16(c.mmix);
        w.u16(c.statx);
        w.u16(c.attr);
        w.u8(c.attr_bit0);
        w.u8(c.dma_mode);
        w.u32(c.key_on);
        w.u32(c.key_off);
    }
    return out;
}

} // namespace dq8::rt::spu2
