// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/iop_memory.h"
#include "support/sif_client.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

namespace rt = dq8::rt;
using dq8::test::IopRig;
using dq8::test::SifClient;

namespace {

constexpr std::uint32_t client = 0x00012000;
constexpr std::uint32_t send = 0x00030000;
constexpr std::uint32_t recv = 0x00031000;

std::uint32_t heap_call(SifClient& c, std::uint32_t fno, std::uint32_t arg) {
    c.write32(send, arg);
    REQUIRE(c.call(client, fno, send, 4, recv, 4));
    return c.read32(recv);
}

} // namespace

TEST_CASE("iopheap: allocations are IOP RAM the SIF accepts as a destination", "[iop][iopheap]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    REQUIRE(c.bind(client, 0x80000003u) != 0);
    const std::uint32_t a = heap_call(c, 1, 0x1004);
    const std::uint32_t b = heap_call(c, 1, 0x800);
    CHECK(a >= rt::IopMemory::heap_base);
    CHECK(b >= a + 0x1004);
    CHECK(a % 16 == 0);
    CHECK(b + 0x800 <= rt::IopMemory::heap_end);

    // Upload into the block the way the game sends sound data, then read it back over the EE's IOP RAM
    // window.
    const auto data = dq8::test::seeded_bytes(21, 0x100);
    c.write(0x00040000, data);
    const std::uint32_t id = c.dma_to_iop(0x00040000, (a + 4 + 3) & ~3u, 0x100);
    REQUIRE(c.wait_for([&] { return c.dma_stat(id) < 0; }));
    std::vector<std::uint8_t> back(0x100);
    for (std::uint32_t i = 0; i < 0x100; i += 4) {
        REQUIRE(rig.m.mmio().read(0x1C000000 + ((a + 4 + 3) & ~3u) + i, 4, back.data() + i, {}));
    }
    CHECK(back == data);

    CHECK(heap_call(c, 2, a) == 0);
    CHECK(heap_call(c, 2, a) == 0xFFFFFFFFu); // not allocated any more
    CHECK(heap_call(c, 2, 0x1234) == 0xFFFFFFFFu);
    CHECK(heap_call(c, 1, 0x1000) == a);     // first fit reuses the freed block
    CHECK(heap_call(c, 1, 0x00400000) == 0); // larger than the heap
}
