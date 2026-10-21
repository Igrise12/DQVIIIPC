// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The GS privileged registers, their vblank phases and the GS kernel calls (session B "Done when": CSR
// FIELD/VSINT/IMR).
#include "runtime/ee/context.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/gs/priv_regs.h"
#include "runtime/kernel/kernel_services.h"
#include "runtime/machine/interrupts.h"
#include "support/ee_rig.h"

#include <catch2/catch_test_macros.hpp>
#include <initializer_list>
#include <string>

using namespace dq8::rt;
using dq8::test::EeRig;

namespace {

constexpr std::uint32_t csr_addr = 0x12001000u;
constexpr std::uint32_t intc_gs = 1u << static_cast<unsigned>(IntcLine::gs);

// Calls a registered kernel service with $a0.. set to `args`; returns $v0.
std::uint64_t call(EeRig& rig, std::string_view name, std::initializer_list<std::uint64_t> args) {
    R5900Context ctx{};
    unsigned reg = 4;
    for (const std::uint64_t a : args) {
        set_gpr_lo(ctx, reg++, a);
    }
    const KernelServiceInfo* service = rig.machine.kernel().find(name);
    REQUIRE(service != nullptr);
    KernelCallContext context(rig.machine, ctx, service->name, name, false, 0, name.front() == 'i');
    rig.machine.kernel().invoke(*service, context);
    return gpr_lo(ctx, 2);
}

} // namespace

TEST_CASE("gs priv: power-on state; only CSR and SIGLBLID read back", "[gs]") {
    EeRig rig;
    const std::uint64_t csr = rig.r64(csr_addr);
    CHECK(((csr >> 14) & 3u) == 1u);       // FIFO empty
    CHECK(((csr >> 16) & 0xFFu) == 0x1Bu); // REV
    CHECK(((csr >> 24) & 0xFFu) == 0x55u); // ID
    CHECK(call(rig, "GsGetIMR", {}) == 0x7F00u);
    rig.w64(0x12000000u, 0x1234u); // PMODE: stored, but reads as CSR
    CHECK(rig.r64(0x12000000u) == csr);
    CHECK(rig.gs().regs().read64(dq8::gs::PrivReg::pmode) == 0x1234u);
    CHECK(rig.r32(csr_addr + 4) == static_cast<std::uint32_t>(csr >> 32));
    CHECK(rig.mem().mmio_log().empty()); // the whole page is handled
}

TEST_CASE("gs priv: FIELD and VSINT at vblank start, with IMR gating INTC 0", "[gs]") {
    EeRig rig;
    InterruptController& intc = rig.machine.interrupts();
    // Before SetGsCrt the mode reads as progressive (CMOD 0): FIELD is set, not toggled.
    rig.machine.fire_vblank_start();
    rig.machine.fire_vblank_start();
    CHECK((rig.gs().csr() & dq8::gs::csr_field) != 0u);
    CHECK((rig.gs().csr() & dq8::gs::csr_vsint) != 0u);
    CHECK((intc.intc_stat() & intc_gs) == 0u); // VSMSK set at power-on

    // GsSetCrt(interlace, NTSC, field mode): FIELD alternates each vblank.
    call(rig, "GsSetCrt", {1, 2, 0});
    CHECK(rig.gs().regs().read64(dq8::gs::PrivReg::smode1) == 0x740814504ull);
    CHECK(rig.gs().regs().read64(dq8::gs::PrivReg::smode2) == 1u);
    CHECK((rig.gs().regs().read64(dq8::gs::PrivReg::syncv) & 1u) == 1u);
    bool field = (rig.gs().csr() & dq8::gs::csr_field) != 0;
    for (int i = 0; i < 4; ++i) {
        rig.machine.fire_vblank_start();
        const bool now = (rig.gs().csr() & dq8::gs::csr_field) != 0;
        CHECK(now != field);
        field = now;
    }
    // The sink saw every vsync, numbered from 0, with the page and the FIELD it had then.
    REQUIRE(rig.sink.vsyncs.size() == 6u);
    for (std::size_t i = 0; i < rig.sink.vsyncs.size(); ++i) {
        CHECK(rig.sink.vsyncs[i].field == i);
        CHECK(rig.sink.vsyncs[i].odd_field ==
              ((rig.sink.vsyncs[i].priv.read64(dq8::gs::PrivReg::csr) & dq8::gs::csr_field) != 0));
    }

    // Unmasking VSINT while it is pending interrupts at once (GsPutIMR returns the old IMR).
    CHECK(call(rig, "GsPutIMR", {0x7F00u & ~(1u << 11)}) == 0x7F00u);
    CHECK((intc.intc_stat() & intc_gs) != 0u);
    CHECK(call(rig, "iGsGetIMR", {}) == 0x7700u);
    // Writing VSINT clears it; the next vblank sets it again and raises INTC 0.
    intc.write_intc_stat(intc_gs);
    rig.w64(csr_addr, dq8::gs::csr_vsint);
    CHECK((rig.gs().csr() & dq8::gs::csr_vsint) == 0u);
    rig.machine.fire_vblank_start();
    CHECK((intc.intc_stat() & intc_gs) != 0u);

    // CSR.RESET clears the page back to power-on.
    rig.w64(csr_addr, dq8::gs::csr_reset);
    CHECK(rig.gs().regs().read64(dq8::gs::PrivReg::smode1) == 0u);
    CHECK(rig.gs().imr() == 0x7F00u);
}

TEST_CASE("gs priv: vblank phases run gs, kernel, then intc", "[gs]") {
    // A sink that looks at the INTC and the SetVSyncFlag target when the gs phase hands it the vsync.
    struct PhaseSink final : dq8::gs::IGsSink {
        void gif_transfer(dq8::gs::GifPath, std::span<const std::uint8_t>) override {}
        void vsync(const dq8::gs::VsyncInfo&) override {
            intc_at_vsync = machine->interrupts().intc_stat();
            flag_at_vsync = machine->memory().load32(0x00100000u);
        }
        std::size_t read_transfer(std::span<std::uint8_t> out) override { return out.size(); }
        dq8::test::FakeMachine* machine = nullptr;
        std::uint32_t intc_at_vsync = 0;
        std::uint32_t flag_at_vsync = 0;
    } sink;
    EeRig rig(sink);
    sink.machine = &rig.machine;
    call(rig, "SetVSyncFlag", {0x00100000u, 0x00100010u});
    rig.machine.fire_vblank_start();
    CHECK(sink.intc_at_vsync == 0u); // INTC 2 comes in the intc phase, after the GS
    CHECK(sink.flag_at_vsync == 0u); // the flag in the kernel phase, after the GS
    CHECK((rig.machine.interrupts().intc_stat() & (1u << 2)) != 0u);
    CHECK(rig.mem().load32(0x00100000u) == 1u);
    rig.machine.fire_vblank_end();
    CHECK((rig.machine.interrupts().intc_stat() & (1u << 3)) != 0u);
}

TEST_CASE("gs priv: SetVSyncFlag writes the flag and the CSR at the next vblank start, once", "[gs]") {
    EeRig rig;
    call(rig, "GsSetCrt", {1, 2, 0});
    rig.mem().store32(0x00200000u, 0xAAAAAAAAu);
    rig.mem().store64(0x00200008u, 0xBBBBBBBBBBBBBBBBull);
    CHECK(call(rig, "SetVSyncFlag", {0x00200000u, 0x00200008u}) == 0u);
    CHECK(rig.mem().load32(0x00200000u) == 0u); // cleared at the call
    CHECK(rig.mem().load64(0x00200008u) == 0u);
    rig.machine.fire_vblank_start();
    CHECK(rig.mem().load32(0x00200000u) == 1u);
    const std::uint64_t saved = rig.mem().load64(0x00200008u);
    CHECK(saved == rig.gs().csr()); // with this field's FIELD bit
    rig.mem().store32(0x00200000u, 0u);
    rig.machine.fire_vblank_start();
    CHECK(rig.mem().load32(0x00200000u) == 0u); // used once
}

TEST_CASE("gs priv: GetGsDxDyOffset and the modes GsSetCrt accepts", "[gs]") {
    EeRig rig;
    call(rig, "GetGsDxDyOffset", {2, 0x00300000u, 0x00300004u, 0x00300008u, 0x0030000Cu});
    CHECK(rig.mem().load32(0x00300000u) == 642u);
    CHECK(rig.mem().load32(0x00300004u) == 25u);
    CHECK(rig.mem().load32(0x00300008u) == 2559u);
    CHECK(rig.mem().load32(0x0030000Cu) == 239u);
    call(rig, "GsSetCrt", {1, 3, 1}); // PAL, frame mode
    CHECK(((rig.gs().regs().read64(dq8::gs::PrivReg::smode1) >> 13) & 3u) == 3u);
    CHECK(rig.gs().regs().read64(dq8::gs::PrivReg::smode2) == 3u);
    CHECK_THROWS_AS(call(rig, "GsSetCrt", {0, 0x50, 0}), GuestStop); // DTV 480p: not modelled
    CHECK_THROWS_AS(call(rig, "GetGsDxDyOffset", {0x1A, 0, 0, 0, 0}), GuestStop);
    // The SDK spelling SetGsCrt resolves to the same service.
    CHECK(rig.machine.kernel().resolve_name("SetGsCrt").service->name == "GsSetCrt");
    REQUIRE_FALSE(rig.machine.emitted.empty());
    CHECK(rig.machine.emitted.back().type == "gs_crt");
}
