// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/error.h"
#include "dq8/core/iso9660.h"
#include "runtime/io/disc_source.h"
#include "support/iso_builder.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <vector>

namespace rt = dq8::rt;
namespace core = dq8::core;

TEST_CASE("disc source: lookup by path in every spelling the IOP sees", "[io][disc]") {
    dq8::test::IsoBuilder b;
    b.add_file("/BIN/TITLE.BIN", dq8::test::seeded_bytes(1, 3000));
    b.add_file("/DMYDATA", dq8::test::seeded_bytes(2, 10));
    b.set_total_sectors(100);
    dq8::test::Layout layout;
    const auto image = core::iso::Image::from_bytes(b.build(&layout));
    const auto disc = rt::make_iso_disc_source(image);

    CHECK(disc->sector_count() == 100);
    for (const char* path : {"BIN/TITLE.BIN", "\\BIN\\TITLE.BIN;1", "/bin/title.bin", "bin\\Title.Bin"}) {
        INFO(path);
        const auto f = disc->find(path);
        REQUIRE(f);
        CHECK(f->path == "BIN/TITLE.BIN");
        CHECK(f->lsn == layout.extent_lba.at("/BIN/TITLE.BIN"));
        CHECK(f->size == 3000);
    }
    REQUIRE(disc->find("\\DMYDATA;1"));
    CHECK(disc->find("\\DMYDATA;1")->size == 10);
    CHECK_FALSE(disc->find("BIN"));
    CHECK_FALSE(disc->find("BIN/NOPE.BIN"));

    std::vector<std::uint8_t> two(2 * 2048);
    disc->read_sectors(layout.extent_lba.at("/BIN/TITLE.BIN"), 2, two);
    const auto title = dq8::test::seeded_bytes(1, 3000);
    CHECK(std::equal(title.begin(), title.end(), two.begin()));
    CHECK_THROWS_AS(disc->read_sectors(99, 2, two), core::Error);
    std::vector<std::uint8_t> wrong(100);
    CHECK_THROWS_AS(disc->read_sectors(0, 1, wrong), core::Error);
    disc->read_sectors(0, 0, std::span<std::uint8_t>{});
}
