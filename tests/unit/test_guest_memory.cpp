// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/error.h"
#include "runtime/ee/context.h"
#include "runtime/ee/elf_loader.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/ee/guest_stop.h"
#include "support/elf_image.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dq8::rt;

TEST_CASE("guest memory: RAM and its mirrors through the host path", "[memory]") {
    GuestMemory m;
    m.store32(0x00100000u, 0xDEADBEEFu);
    CHECK(m.load32(0x80100000u) == 0xDEADBEEFu); // kseg0
    CHECK(m.load32(0xA0100000u) == 0xDEADBEEFu); // kseg1
    CHECK(m.load32(0x20100000u) == 0xDEADBEEFu); // uncached mirror
    CHECK(m.load16(0x00100002u) == 0xDEADu);
    CHECK(m.load8(0x00100003u) == 0xDEu);
    std::uint32_t raw = 0;
    std::memcpy(&raw, m.rdram() + 0x00100000u, sizeof raw);
    CHECK(raw == 0xDEADBEEFu); // the same bytes the generated fast path reads
    m.store128(0x00100010u, 0x0102030405060708ull, 0x1112131415161718ull);
    std::uint64_t q[2] = {};
    m.load128(0x00100010u, q);
    CHECK(q[0] == 0x0102030405060708ull);
    CHECK(q[1] == 0x1112131415161718ull);
    m.store64(0x01FFFFFCu, 0xAABBCCDD11223344ull); // wraps at the end of RAM, as the fast path does
    CHECK(m.rdram()[0] == 0xDD);
    CHECK(m.load64(0x01FFFFFCu) == 0xAABBCCDD11223344ull);
    CHECK(m.mmio_log().empty());
}

TEST_CASE("guest memory: the scratchpad is separate from RAM", "[memory]") {
    GuestMemory m;
    m.store32(0x70000010u, 0x12345678u);
    CHECK(m.load32(0x70000010u) == 0x12345678u);
    std::uint32_t sp = 0;
    std::memcpy(&sp, m.scratchpad() + 0x10, sizeof sp);
    CHECK(sp == 0x12345678u);
    CHECK(m.load32(0x00000010u) == 0u);
    m.store16(0x70003FFFu, 0xBBAAu); // wraps inside the scratchpad
    CHECK(m.scratchpad()[0x3FFF] == 0xAA);
    CHECK(m.scratchpad()[0] == 0xBB);
    CHECK(m.mmio_log().empty());
}

TEST_CASE("guest memory: device ranges are logged holes", "[memory]") {
    GuestMemory m;
    std::vector<MmioAccess> sunk;
    m.set_mmio_sink([&](const MmioAccess& a) { sunk.push_back(a); });

    CHECK(m.load32(0x10003000u, 0x00100100u) == 0u);
    m.store32(0x10003000u, 0x55u, 0x00100104u); // dropped; same address, counted, not sunk again
    CHECK(m.load32(0x10003000u) == 0u);
    m.store64(0x12001000u, 0x0102030405060708ull, 0x00100108u);
    CHECK(m.load8(0xBFC00000u) == 0u);
    CHECK(m.load32(0x11000000u) == 0u);
    CHECK(m.load32(0xBC000000u) == 0u); // IOP RAM through kseg1
    CHECK(m.load32(0xF0000000u) == 0u); // kseg3: nothing mapped

    const auto& log = m.mmio_log();
    REQUIRE(log.size() == 6);
    CHECK(sunk.size() == 6);
    CHECK(log[0].address == 0x10003000u);
    CHECK(log[0].region == MmioRegion::io);
    CHECK_FALSE(log[0].store);
    CHECK(log[0].width == 4);
    CHECK(log[0].pc == 0x00100100u);
    CHECK(log[0].count == 3);
    CHECK(log[1].region == MmioRegion::gs_privileged);
    CHECK(log[1].store);
    CHECK(log[1].value == 0x0102030405060708ull);
    CHECK(log[2].region == MmioRegion::bios);
    CHECK(log[3].region == MmioRegion::vu);
    CHECK(log[4].region == MmioRegion::iop_ram);
    CHECK(log[5].region == MmioRegion::unmapped);
    CHECK(m.mmio_access_count() == 8);
    CHECK(to_string(MmioRegion::gs_privileged) == "gs-privileged");
}

TEST_CASE("guest memory: trap-mmio stops at the first device access", "[memory]") {
    GuestMemory m;
    m.set_trap_mmio(true);
    m.store32(0x00100000u, 1u); // RAM is never trapped
    m.store32(0x70000000u, 1u); // nor the scratchpad
    try {
        m.store32(0x1000F000u, 0x1u, 0x00100200u);
        FAIL("no GuestStop");
    } catch (const GuestStop& stop) {
        CHECK(stop.info().reason == StopReason::mmio_trap);
        CHECK(stop.info().address == 0x1000F000u);
        CHECK(stop.info().pc == 0x00100200u);
        CHECK(stop.info().message.find("store32 at 0x1000F000 (io)") != std::string::npos);
    }
    CHECK(m.mmio_log().size() == 1); // recorded before the stop
}

TEST_CASE("guest memory: host byte copies refuse device ranges", "[memory]") {
    GuestMemory m;
    const std::vector<std::uint8_t> bytes{1, 2, 3, 4};
    m.write_bytes(0x80200000u, bytes);
    std::vector<std::uint8_t> back(4);
    m.read_bytes(0x00200000u, back);
    CHECK(back == bytes);
    m.fill(0x70000000u, 0x4000u, 0xCC);
    CHECK(m.scratchpad()[0x3FFF] == 0xCC);
    CHECK_THROWS_AS(m.write_bytes(0x10000000u, bytes), std::out_of_range);
    CHECK_THROWS_AS(m.fill(0x01FFFFFEu, 4, 0), std::out_of_range); // would wrap
    CHECK_THROWS_AS(m.fill(0x70003FFEu, 4, 0), std::out_of_range);
}

TEST_CASE("elf loader: copies filesz, zeroes the rest, including zero-filesize segments", "[memory][elf]") {
    GuestMemory m;
    m.fill(0x00100000u, 0x400, 0xAA); // stale bytes the loader must clear
    m.fill(0x00300000u, 0x100, 0xAA);
    const std::vector<std::uint8_t> code{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    const std::vector<std::uint8_t> elf =
        test_elf::build(0x00100008u, {test_elf::Segment{0x00100000u, code, 0x200, 5},
                                      test_elf::Segment{0x00300000u, {}, 0x80, 6},
                                      test_elf::Segment{0x00300000u, {}, 0x40, 6}});
    const LoadedElf loaded = load_elf(elf, m);
    CHECK(loaded.entry == 0x00100008u);
    REQUIRE(loaded.segments.size() == 3);
    CHECK(loaded.segments[0].filesz == 6);
    CHECK(loaded.segments[0].memsz == 0x200);
    CHECK(std::memcmp(m.rdram() + 0x00100000u, code.data(), code.size()) == 0);
    CHECK(m.rdram()[0x00100006u] == 0);
    CHECK(m.rdram()[0x001001FFu] == 0);
    CHECK(m.rdram()[0x00100200u] == 0xAA); // beyond memsz: untouched
    CHECK(m.rdram()[0x00300000u] == 0);
    CHECK(m.rdram()[0x0030007Fu] == 0);
    CHECK(m.rdram()[0x00300080u] == 0xAA);
}

TEST_CASE("elf loader: refuses what it cannot place", "[memory][elf]") {
    GuestMemory m;
    using dq8::core::Error;
    const auto seg = [](std::uint32_t vaddr, std::uint32_t memsz) {
        return test_elf::Segment{vaddr, std::vector<std::uint8_t>(4, 0), memsz, 7};
    };
    CHECK_THROWS_AS(load_elf(test_elf::build(0, {seg(0x00100000u, 8)}, 3 /* EM_386 */), m), Error);
    CHECK_THROWS_AS(load_elf(test_elf::build(0, {seg(0x00100000u, 8)}, 8, 3 /* ET_DYN */), m), Error);
    CHECK_THROWS_AS(load_elf(test_elf::build(0, {seg(0x01FFFFF0u, 0x20)}), m), Error); // past RAM
    CHECK_THROWS_AS(load_elf(test_elf::build(0, {seg(0x10000000u, 8)}), m), Error);    // a device range
    std::vector<std::uint8_t> truncated = test_elf::build(0, {seg(0x00100000u, 8)});
    truncated.resize(truncated.size() - 2);
    CHECK_THROWS_AS(load_elf(truncated, m), Error);
    CHECK_THROWS_AS(load_elf(test_elf::build(0, {test_elf::Segment{0x00100000u, {}, 0, 7}}), m),
                    Error); // empty
    // The scratchpad is a valid target.
    CHECK(load_elf(test_elf::build(0x70000000u, {seg(0x70000000u, 0x10)}), m).segments.size() == 1);
}

TEST_CASE("context: hand-off state and dump", "[memory][context]") {
    R5900Context ctx;
    set_gpr_lo(ctx, 4, 0x55);
    reset_context(ctx, 0x00100008u);
    CHECK(ctx.pc == 0x00100008u);
    CHECK(gpr_lo(ctx, 29) == initial_stack_pointer);
    CHECK(gpr_lo(ctx, 4) == 0);
    CHECK(gpr_lo(ctx, 5) == 0);
    CHECK(gpr_lo(ctx, 31) == 0);
    CHECK(ctx.cop0_status == 0x00010001u); // R5900Context's power-on state is kept
    set_gpr_lo(ctx, 0, 0x99);              // $zero stays zero
    CHECK(gpr_lo(ctx, 0) == 0);
    set_gpr_lo(ctx, 2, 0x12345679u);
    std::uint64_t hi = 0xFEEDu;
    std::memcpy(reinterpret_cast<std::uint8_t*>(&ctx.r[3]) + 8, &hi, sizeof hi);
    CHECK(gpr_hi(ctx, 3) == 0xFEEDu);
    const std::string dump = format_context(ctx);
    CHECK(dump.find("pc   00100008") != std::string::npos);
    CHECK(dump.find("v0   0000000012345679") != std::string::npos);
    CHECK(dump.find("v1   000000000000FEED_0000000000000000") != std::string::npos);
    CHECK(dump.find("sp   0000000001FFFFF0") != std::string::npos);
    CHECK(gpr_names[31] == "ra");
}
