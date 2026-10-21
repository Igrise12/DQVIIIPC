// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PCSX2 Dev Team
// Register behaviour adapted from PCSX2 (GPL-3.0-or-later): pcsx2/GS.cpp (gsCSRwrite, IMRwrite, the
// CSR-mirroring reads), pcsx2/GS.h (tGS_CSR::Reset, tGS_IMR::reset) and pcsx2/Counters.cpp (GSVSync,
// IsProgressiveVideoMode), PCSX2 commit 5b6e6e2.
#include "runtime/gs/gs_priv_device.h"

#include "runtime/ee/guest_memory.h"
#include "runtime/gs/gs_sink.h"
#include "runtime/kernel/kernel_services.h"
#include "runtime/machine/interrupts.h"

#include <cstring>
#include <string>

namespace dq8::rt {

namespace {

using gs::PrivReg;

constexpr std::uint32_t off(PrivReg reg) noexcept {
    return static_cast<std::uint32_t>(reg);
}

// CSR after power-on or a CSR.RESET write: FIFO empty (bits 14-15 = 01), REV 0x1B, ID 0x55 (tGS_CSR::Reset).
constexpr std::uint64_t csr_power_on = (1ull << 14) | (0x1Bull << 16) | (0x55ull << 24);
// The CSR bits a written 1 clears (SIGNAL, FINISH, HSINT, VSINT, EDWINT).
constexpr std::uint64_t csr_w1c =
    gs::csr_signal | gs::csr_finish | gs::csr_hsint | gs::csr_vsint | gs::csr_edwint;
// IMR after reset: all five causes masked, bits 13-14 read as 1 (tGS_IMR::reset).
constexpr std::uint64_t imr_power_on = 0x7F00u;

// SMODE1 from its fields (layout as in PCSX2's GSRegSMODE1).
struct Smode1Fields {
    std::uint64_t rc, lc, t1248, slck, cmod, ex, prst, sint, xpck, pck2, spml, gcont, phs, pvs, pehs, pevs,
        clksel, nvck, slck2, vcksel, vhp;
};
constexpr std::uint64_t smode1(const Smode1Fields& f) noexcept {
    return f.rc | f.lc << 3 | f.t1248 << 10 | f.slck << 12 | f.cmod << 13 | f.ex << 15 | f.prst << 16 |
           f.sint << 17 | f.xpck << 18 | f.pck2 << 19 | f.spml << 21 | f.gcont << 25 | f.phs << 26 |
           f.pvs << 27 | f.pehs << 28 | f.pevs << 29 | f.clksel << 30 | f.nvck << 32 | f.slck2 << 33 |
           f.vcksel << 34 | f.vhp << 36;
}
// The field values PCSX2 documents for NTSC and PAL in the comment after GSRegSMODE1 (pcsx2/GS/GSRegs.h).
constexpr std::uint64_t smode1_ntsc =
    smode1({4, 32, 1, 0, 2, 0, 1, 0, 0, 0, 4, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0});
constexpr std::uint64_t smode1_pal = smode1({4, 32, 1, 0, 3, 0, 1, 0, 0, 0, 4, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0});
static_assert(smode1_ntsc == 0x740814504ull);

// SYNCV from its fields (VFP, VFPE, VBP, VBPE, VDP, VS). The values are the NTSC and PAL vertical timings
// in half lines; inferred, to be confirmed against the settings-screen dump in bring-up (acceptance 5).
// What matters to the machine is VFP's low bit: with CMOD set, an odd VFP makes CSR FIELD alternate.
constexpr std::uint64_t syncv(std::uint64_t vfp, std::uint64_t vfpe, std::uint64_t vbp, std::uint64_t vbpe,
                              std::uint64_t vdp, std::uint64_t vs) noexcept {
    return vfp | vfpe << 10 | vbp << 20 | vbpe << 32 | vdp << 42 | vs << 53;
}
constexpr std::uint64_t syncv_ntsc = syncv(1, 6, 26, 6, 480, 6);
constexpr std::uint64_t syncv_pal = syncv(1, 5, 33, 5, 576, 5);

// GetGsDxDyOffset: the display origin and size for a mode, from PCSX2's VideoModeOffsets and
// VideoModeDividers (pcsx2/GS/GSState.cpp). Inferred; to be confirmed against the DISPLAY registers of the
// settings-screen dump in bring-up.
struct DisplayOffsets {
    std::int32_t dx, dy, dw, dh;
};
constexpr DisplayOffsets offsets_ntsc{642, 25, 2559, 239};
constexpr DisplayOffsets offsets_pal{676, 36, 2559, 287};

bool is_ntsc(std::uint32_t mode) noexcept {
    return mode == 0x00u || mode == 0x02u;
}
bool is_pal(std::uint32_t mode) noexcept {
    return mode == 0x01u || mode == 0x03u;
}

} // namespace

void GsPrivDevice::PhaseListener::on_vblank_start(std::uint64_t field) {
    if (phase_ == VblankPhase::gs) {
        device_.vblank_gs(field);
    } else {
        device_.vblank_kernel();
    }
}

void GsPrivDevice::attach(MachineServices& machine) {
    machine_ = &machine;
    machine.mmio().map(base, static_cast<std::uint32_t>(gs::priv_page_size), *this);
    machine.add_vblank_listener(VblankPhase::gs, gs_phase_);
    machine.add_vblank_listener(VblankPhase::kernel, kernel_phase_);
    register_services(machine);
}

void GsPrivDevice::reset() {
    regs_ = gs::PrivRegs{};
    regs_.write64(PrivReg::csr, csr_power_on);
    regs_.write64(PrivReg::imr, imr_power_on);
    armed_ = false;
    vsync_flag_ = 0;
    vsync_csr_ = 0;
}

bool GsPrivDevice::read(std::uint32_t offset, std::uint32_t width, std::uint8_t* out, const MmioContext& at) {
    // Only SIGLBLID and CSR read back; every other address reads as CSR (pcsx2/GS.cpp gsRead*).
    const std::uint32_t o = offset & 0x13FFu;
    std::uint8_t slot[16] = {};
    const std::uint32_t source =
        (o & ~0xFu) == off(PrivReg::siglblid) ? off(PrivReg::siglblid) : off(PrivReg::csr);
    std::memcpy(slot, regs_.page.data() + source, 8);
    std::memcpy(out, slot + (o & 0x7u), width <= 8 ? width : 8);
    if (width > 8) {
        std::memset(out + 8, 0, width - 8);
    }
    std::uint64_t value = 0;
    std::memcpy(&value, out, width < 8 ? width : 8);
    machine_->note_poll(at.pc, base + offset, value);
    return true;
}

bool GsPrivDevice::write(std::uint32_t offset, std::uint32_t width, const std::uint8_t* in,
                         const MmioContext& /*at*/) {
    const std::uint32_t o = offset & 0x13FFu;
    const std::uint32_t reg = o & ~0xFu;
    const std::uint32_t within = o & 0xFu;
    if (within >= 8) {
        return true; // the upper half of a 16-byte slot holds no register
    }
    const std::uint32_t bytes = width - (within + width > 8 ? within + width - 8 : 0);
    std::uint64_t value = 0;
    std::memcpy(reinterpret_cast<std::uint8_t*>(&value) + within, in, bytes);
    const std::uint64_t mask = bytes >= 8 ? ~0ull : (((1ull << (bytes * 8)) - 1) << (within * 8));
    if (reg == off(PrivReg::csr)) {
        write_csr(value & mask);
    } else if (reg == off(PrivReg::imr)) {
        const std::uint64_t old = regs_.read64(PrivReg::imr);
        write_imr((old & ~mask) | (value & mask));
    } else {
        std::uint64_t old = 0;
        std::memcpy(&old, regs_.page.data() + reg, 8);
        const std::uint64_t merged = (old & ~mask) | (value & mask);
        std::memcpy(regs_.page.data() + reg, &merged, 8);
    }
    machine_->note_activity();
    return true;
}

void GsPrivDevice::write_csr(std::uint64_t value) {
    if ((value & gs::csr_reset) != 0) {
        // RESET clears the privileged registers too (gsCSRwrite).
        regs_ = gs::PrivRegs{};
        regs_.write64(PrivReg::csr, csr_power_on);
        regs_.write64(PrivReg::imr, imr_power_on);
    }
    std::uint64_t csr = regs_.read64(PrivReg::csr);
    csr &= ~(value & csr_w1c);
    regs_.write64(PrivReg::csr, csr);
}

void GsPrivDevice::write_imr(std::uint64_t value) {
    const std::uint64_t old = regs_.read64(PrivReg::imr);
    const std::uint64_t csr = regs_.read64(PrivReg::csr);
    // A cause that is set and becomes unmasked interrupts at once (IMRwrite).
    if ((csr & 0x1Fu) & ((~value & old) >> 8)) {
        machine_->interrupts().raise_intc(IntcLine::gs);
    }
    regs_.write64(PrivReg::imr, (value & 0x1F00u) | 0x6000u);
}

void GsPrivDevice::raise_if_unmasked(std::uint64_t csr_bit, std::uint64_t imr_bit) {
    regs_.write64(PrivReg::csr, regs_.read64(PrivReg::csr) | csr_bit);
    if ((regs_.read64(PrivReg::imr) & imr_bit) == 0) {
        machine_->interrupts().raise_intc(IntcLine::gs);
    }
    machine_->note_activity();
}

void GsPrivDevice::signal(std::uint64_t data) {
    // SIGLBLID.SIGID takes the ID bits selected by IDMSK (the upper word of the SIGNAL data).
    const auto id = static_cast<std::uint32_t>(data);
    const auto mask = static_cast<std::uint32_t>(data >> 32);
    std::uint64_t sl = regs_.read64(PrivReg::siglblid);
    const std::uint32_t sigid = (static_cast<std::uint32_t>(sl) & ~mask) | (id & mask);
    sl = (sl & 0xFFFFFFFF00000000ull) | sigid;
    regs_.write64(PrivReg::siglblid, sl);
    raise_if_unmasked(gs::csr_signal, imr_sigmsk);
}

void GsPrivDevice::finish() {
    raise_if_unmasked(gs::csr_finish, imr_finishmsk);
}

void GsPrivDevice::label(std::uint64_t data) {
    const auto id = static_cast<std::uint32_t>(data);
    const auto mask = static_cast<std::uint32_t>(data >> 32);
    std::uint64_t sl = regs_.read64(PrivReg::siglblid);
    const std::uint32_t lblid = (static_cast<std::uint32_t>(sl >> 32) & ~mask) | (id & mask);
    sl = (sl & 0xFFFFFFFFull) | (static_cast<std::uint64_t>(lblid) << 32);
    regs_.write64(PrivReg::siglblid, sl);
    machine_->note_activity();
}

void GsPrivDevice::vblank_gs(std::uint64_t field) {
    const std::uint64_t smode1_value = regs_.read64(PrivReg::smode1);
    // SMODE1.SINT holds the sync generator: no field change and no VSINT (GSVSync).
    if ((smode1_value & (1ull << 17)) == 0) {
        std::uint64_t csr = regs_.read64(PrivReg::csr);
        const bool progressive = (regs_.read64(PrivReg::syncv) & 1u) == 0 ||
                                 (smode1_value & 0x6000u) == 0; // IsProgressiveVideoMode
        if (progressive) {
            csr |= gs::csr_field;
        } else {
            csr ^= gs::csr_field;
        }
        regs_.write64(PrivReg::csr, csr);
        if ((csr & gs::csr_vsint) == 0) {
            raise_if_unmasked(gs::csr_vsint, imr_vsmsk);
        }
    }
    gs::VsyncInfo info;
    info.field = field;
    info.odd_field = (regs_.read64(PrivReg::csr) & gs::csr_field) != 0;
    info.priv = regs_;
    sink_.vsync(info);
}

void GsPrivDevice::vblank_kernel() {
    if (!armed_) {
        return;
    }
    // The kernel's vblank-start work for SetVSyncFlag: the flag becomes 1 and the CSR (with this field's
    // FIELD bit) is saved; the targets are used once.
    armed_ = false;
    GuestMemory& mem = machine_->memory();
    if (vsync_flag_ != 0) {
        mem.store32(vsync_flag_, 1u);
    }
    if (vsync_csr_ != 0) {
        mem.store64(vsync_csr_, regs_.read64(PrivReg::csr));
    }
    machine_->note_activity();
}

void GsPrivDevice::register_services(MachineServices& machine) {
    KernelServices& k = machine.kernel();
    const std::string owner(name());

    k.add({"GsSetCrt",
           [this](KernelCallContext& call) {
               const std::uint32_t interlace = call.arg(0) & 1u;
               const std::uint32_t mode = call.arg(1);
               const std::uint32_t ffmd = call.arg(2) & 1u;
               if (!is_ntsc(mode) && !is_pal(mode)) {
                   call.unimplemented("video mode " + hex32(mode) + " (only NTSC and PAL are modelled)");
               }
               regs_.write64(PrivReg::smode1, is_ntsc(mode) ? smode1_ntsc : smode1_pal);
               regs_.write64(PrivReg::smode2, interlace | (ffmd << 1));
               regs_.write64(PrivReg::syncv, is_ntsc(mode) ? syncv_ntsc : syncv_pal);
               machine_->emit(MachineEvent{"gs_crt", {}}
                                  .add("interlace", std::uint64_t{interlace})
                                  .add("mode", std::uint64_t{mode})
                                  .add("ffmd", std::uint64_t{ffmd}));
               machine_->note_activity();
           },
           0, "", owner});

    auto get_imr = [this](KernelCallContext& call) { call.ret64(regs_.read64(PrivReg::imr)); };
    auto put_imr = [this](KernelCallContext& call) {
        const std::uint64_t old = regs_.read64(PrivReg::imr);
        write_imr(call.arg64(0));
        call.ret64(old);
        machine_->note_activity();
    };
    k.add({"GsGetIMR", get_imr, 0, "", owner});
    k.add({"iGsGetIMR", get_imr, kernel_service_interrupt_ok, "", owner});
    k.add({"GsPutIMR", put_imr, 0, "", owner});
    k.add({"iGsPutIMR", put_imr, kernel_service_interrupt_ok, "", owner});

    k.add({"SetVSyncFlag",
           [this](KernelCallContext& call) {
               vsync_flag_ = call.arg(0);
               vsync_csr_ = call.arg(1);
               armed_ = true;
               // Clear the targets now so the caller waits for the next vblank (as upstream does).
               if (vsync_flag_ != 0) {
                   call.mem().store32(vsync_flag_, 0u);
               }
               if (vsync_csr_ != 0) {
                   call.mem().store64(vsync_csr_, 0u);
               }
               call.ret(0);
           },
           0, "", owner});

    k.add({"GetGsDxDyOffset",
           [](KernelCallContext& call) {
               const std::uint32_t mode = call.arg(0);
               if (!is_ntsc(mode) && !is_pal(mode)) {
                   call.unimplemented("video mode " + hex32(mode) + " (only NTSC and PAL are modelled)");
               }
               const DisplayOffsets& o = is_ntsc(mode) ? offsets_ntsc : offsets_pal;
               const std::int32_t values[4] = {o.dx, o.dy, o.dw, o.dh};
               for (unsigned i = 0; i < 4; ++i) {
                   if (const std::uint32_t target = call.arg(1 + i); target != 0) {
                       call.mem().store32(target, static_cast<std::uint32_t>(values[i]));
                   }
               }
           },
           0, "", owner});
}

} // namespace dq8::rt
