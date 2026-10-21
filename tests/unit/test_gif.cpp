// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The GIF (session B "Done when": packet cutting per path, the SIGNAL/FINISH/LABEL/TRXDIR scan).
#include "runtime/gs/priv_regs.h"
#include "runtime/machine/interrupts.h"
#include "support/ee_rig.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using namespace dq8::rt;
using dq8::gs::GifPath;
using dq8::test::EeRig;
using dq8::test::gif_tag;
using dq8::test::put32;
using dq8::test::put64;
using dq8::test::vif_code;

namespace {

void qword(std::vector<std::uint8_t>& out, std::uint64_t lo, std::uint64_t hi) {
    put64(out, lo);
    put64(out, hi);
}
void ad(std::vector<std::uint8_t>& out, std::uint8_t reg, std::uint64_t value) {
    qword(out, value, reg);
}

constexpr std::uint32_t intc_gs = 1u << static_cast<unsigned>(IntcLine::gs);

} // namespace

TEST_CASE("gif: packets are cut at EOP whatever the chunking", "[gif]") {
    // PACKED (2 loops x 3 registers), REGLIST (3 x 1, padded to 2 quadwords), IMAGE (2): one packet whose
    // last tag has EOP, then a second one-tag packet.
    std::vector<std::uint8_t> stream;
    qword(stream, gif_tag(2, 0, 3, false), 0x510);
    for (unsigned i = 0; i < 6; ++i) {
        qword(stream, i, 0);
    }
    qword(stream, gif_tag(3, 1, 1, false), 0x1);
    qword(stream, 1, 2);
    qword(stream, 3, 0);
    qword(stream, gif_tag(2, 2, 0, true), 0);
    qword(stream, 0xAAAA, 0xBBBB);
    qword(stream, 0xCCCC, 0xDDDD);
    const std::size_t first_size = stream.size();
    qword(stream, gif_tag(0, 2, 0, true), 0x99);

    for (const std::size_t chunk : {std::size_t{16}, std::size_t{48}, stream.size()}) {
        EeRig rig;
        for (std::size_t at = 0; at < stream.size(); at += chunk) {
            const std::size_t n = std::min(chunk, stream.size() - at);
            rig.gif().path_data(GifPath::path3, std::span(stream).subspan(at, n));
        }
        REQUIRE(rig.sink.transfers.size() == 2u);
        CHECK(rig.sink.transfers[0].bytes.size() == first_size);
        CHECK(std::memcmp(rig.sink.transfers[0].bytes.data(), stream.data(), first_size) == 0);
        CHECK(rig.sink.transfers[1].bytes.size() == 16u);
        CHECK(rig.gif().stats(GifPath::path3).packets == 2u);
        CHECK(rig.gif().stats(GifPath::path3).bytes == stream.size());
    }
}

TEST_CASE("gif: paths are cut independently", "[gif]") {
    EeRig rig;
    std::vector<std::uint8_t> head;
    qword(head, gif_tag(1, 2, 0, true), 0);
    std::vector<std::uint8_t> body;
    qword(body, 0x1234, 0x5678);
    std::vector<std::uint8_t> other;
    qword(other, gif_tag(0, 0, 1, true), 0x42);
    rig.gif().path_data(GifPath::path2, head);
    rig.gif().path_data(GifPath::path3, other); // completes while PATH2 waits for its data
    rig.gif().path_data(GifPath::path2, body);
    REQUIRE(rig.sink.transfers.size() == 2u);
    CHECK(rig.sink.transfers[0].path == GifPath::path3);
    CHECK(rig.sink.transfers[1].path == GifPath::path2);
    CHECK(rig.sink.transfers[1].bytes.size() == 32u);
}

TEST_CASE("gif: SIGNAL, FINISH and LABEL update the privileged registers and INTC 0", "[gif][gs]") {
    EeRig rig;
    InterruptController& intc = rig.machine.interrupts();
    std::vector<std::uint8_t> p;
    qword(p, gif_tag(3, 0, 1, true), 0xE);
    ad(p, 0x60, 0x0000FFFF00001234ull); // SIGNAL: ID 0x1234, IDMSK 0xFFFF
    ad(p, 0x62, 0xFFFF0000ABCD0000ull); // LABEL: ID 0xABCD0000, IDMSK 0xFFFF0000
    ad(p, 0x61, 0);                     // FINISH
    rig.gif().path_data(GifPath::path2, p);
    const std::uint64_t csr = rig.gs().csr();
    CHECK((csr & dq8::gs::csr_signal) != 0u);
    CHECK((csr & dq8::gs::csr_finish) != 0u);
    const std::uint64_t sl = rig.r64(0x12001080u);
    CHECK((sl & 0xFFFFFFFFu) == 0x1234u);
    CHECK((sl >> 32) == 0xABCD0000u);
    CHECK((intc.intc_stat() & intc_gs) == 0u); // IMR masks every cause at power-on

    // Unmasking a pending cause interrupts at once; clearing the CSR bit (write 1) clears the cause.
    rig.w64(0x12001010u, 0x7F00u & ~(1u << 8));
    CHECK((intc.intc_stat() & intc_gs) != 0u);
    rig.w64(0x12001000u, dq8::gs::csr_signal | dq8::gs::csr_finish);
    CHECK((rig.gs().csr() & (dq8::gs::csr_signal | dq8::gs::csr_finish)) == 0u);

    // With FINISH unmasked, a FINISH raises INTC 0 directly.
    intc.write_intc_stat(intc_gs);
    rig.w64(0x12001010u, 0x7F00u & ~(1u << 9));
    std::vector<std::uint8_t> f;
    qword(f, gif_tag(1, 0, 1, true), 0xE);
    ad(f, 0x61, 0);
    rig.gif().path_data(GifPath::path1, f);
    CHECK((intc.intc_stat() & intc_gs) != 0u);

    // A SIGNAL in a PACKED descriptor other than A+D, or in REGLIST, is not a register write.
    EeRig plain;
    std::vector<std::uint8_t> q;
    qword(q, gif_tag(1, 0, 1, true), 0x0); // PRIM descriptor
    ad(q, 0x60, 0x1);
    plain.gif().path_data(GifPath::path2, q);
    CHECK((plain.gs().csr() & dq8::gs::csr_signal) == 0u);
}

TEST_CASE("gif: TRXDIR = 1 arms a readback that VIF1 FIFO reads drain through the sink", "[gif][vif]") {
    EeRig rig;
    std::vector<std::uint8_t> p;
    qword(p, gif_tag(3, 0, 1, true), 0xE);
    ad(p, 0x50, 0x00ull << 24);       // BITBLTBUF: source PSMCT32
    ad(p, 0x52, 4ull | (2ull << 32)); // TRXREG: 4 x 2
    ad(p, 0x53, 1);                   // TRXDIR: local -> host
    rig.gif().path_data(GifPath::path2, p);
    CHECK(rig.gif().readback_pending() == 32u);
    std::uint64_t q[2] = {};
    rig.mem().load128(0x10005000u, q);
    CHECK((q[0] & 0xFFu) == 0u);
    CHECK(((q[1] >> 56) & 0xFFu) == 15u); // the sink's pattern
    rig.mem().load128(0x10005000u, q);
    CHECK((q[0] & 0xFFu) == 16u);
    CHECK(rig.gif().readback_pending() == 0u);
    CHECK(rig.sink.reads == 2u);

    // PSMT8 and PSMT4 transfers: bits per pixel, rounded up to whole quadwords.
    std::vector<std::uint8_t> t8;
    qword(t8, gif_tag(3, 0, 1, true), 0xE);
    ad(t8, 0x50, 0x13ull << 24);
    ad(t8, 0x52, 5ull | (3ull << 32));
    ad(t8, 0x53, 1);
    rig.gif().path_data(GifPath::path3, t8);
    CHECK(rig.gif().readback_pending() == 16u); // 15 bytes
}

TEST_CASE("gif: FIFO writes are PATH3; RST drops a partial packet; M3R masks PATH3", "[gif]") {
    EeRig rig;
    std::uint8_t q[16] = {};
    const std::uint64_t head = gif_tag(1, 2, 0, true);
    std::memcpy(q, &head, 8);
    rig.w128(0x10006000u, q); // tag only: incomplete
    rig.w32(0x10003000u, 1u); // GIF_CTRL.RST
    const std::uint64_t whole = gif_tag(0, 0, 1, true);
    std::memcpy(q, &whole, 8);
    rig.w128(0x10006000u, q);
    REQUIRE(rig.sink.transfers.size() == 1u); // only the packet after the reset
    CHECK(rig.sink.transfers[0].path == GifPath::path3);
    CHECK(rig.sink.transfers[0].bytes.size() == 16u);

    rig.w32(0x10003010u, 1u); // GIF_MODE.M3R
    rig.w128(0x10006000u, q);
    CHECK(rig.sink.transfers.size() == 1u);
    CHECK((rig.r32(0x10003020u) & (Gif::stat_m3r | Gif::stat_p3q)) == (Gif::stat_m3r | Gif::stat_p3q));
    rig.w32(0x10003010u, 0u);
    CHECK(rig.sink.transfers.size() == 2u);
    // VIF1 MSKPATH3 and M3R are independent masks: both must be off.
    rig.vif1().push(std::vector<std::uint8_t>{0x00, 0x80, 0x00, 0x06});
    rig.w32(0x10003010u, 1u);
    rig.w32(0x10003010u, 0u);
    rig.w128(0x10006000u, q);
    CHECK(rig.sink.transfers.size() == 2u);
    std::vector<std::uint8_t> unmask;
    put32(unmask, vif_code(0x06, 0, 0));
    rig.vif1().push(unmask);
    CHECK(rig.sink.transfers.size() == 3u);
}
