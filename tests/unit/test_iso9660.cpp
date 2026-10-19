// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/error.h"
#include "dq8/core/iso9660.h"
#include "support/iso_builder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstring>

using dq8::core::Error;
using dq8::core::ErrorCode;
using dq8::core::iso::Image;
using dq8::test::IsoBuilder;
using dq8::test::Layout;
using dq8::test::seeded_bytes;

namespace {

IsoBuilder sample_disc() {
    IsoBuilder b;
    b.set_system_identifier("PLAYSTATION");
    b.add_file("/SYSTEM.CNF", dq8::test::seeded_bytes(1, 57));
    b.add_file("/BOOT.ELF", seeded_bytes(2, 5000));      // spans three sectors
    b.add_file("/NOEXT", seeded_bytes(3, 10));           // recorded as "NOEXT.;1"
    b.add_file("/EMPTY.TXT", {});                        // zero-length file
    b.add_file("/BIN/TITLE.BIN", seeded_bytes(4, 2048)); // exactly one sector
    b.add_file("/BIN/MENU.BIN", seeded_bytes(5, 2049));  // one byte over
    b.add_file("/MOVIE/EV/URA.MVI", seeded_bytes(6, 100));
    b.add_file("/MOVIE/E904/A1.MVI", seeded_bytes(7, 100));
    b.add_directory("/MODULES"); // empty directory
    return b;
}

ErrorCode code_of(const std::vector<std::uint8_t>& image) {
    try {
        (void)Image::from_bytes(image);
    } catch (const Error& e) {
        return e.code();
    }
    // FAIL_CHECK records the failure without throwing, so the return stays reachable (MSVC C4702 under /WX).
    FAIL_CHECK("expected the image to be rejected");
    return ErrorCode::io_open;
}

} // namespace

TEST_CASE("volume descriptors are scanned and the primary one is parsed", "[core][iso]") {
    Layout lay;
    const auto bytes = sample_disc().build(&lay);
    const Image image = Image::from_bytes(bytes);
    CHECK(image.image_size() == bytes.size());
    CHECK(image.image_sectors() == bytes.size() / 2048);
    CHECK(image.volume_sectors() == image.image_sectors());
    CHECK(image.volume().system_identifier == "PLAYSTATION");
    CHECK(image.volume().volume_identifier == "SYNTHETIC");
    CHECK(image.volume().logical_block_size == 2048);
    CHECK(image.volume().descriptor_count == 2);
    CHECK(image.volume().root_lba == lay.root_lba);
    CHECK(image.volume().path_table_lba_l == lay.path_table_l_lba);
    CHECK(image.volume().path_table_lba_m == lay.path_table_m_lba);
    CHECK(image.volume().path_table_size == lay.path_table_size);
    CHECK(image.volume().path_table_directories == 6); // root, BIN, MODULES, MOVIE, MOVIE/E904, MOVIE/EV
}

TEST_CASE("the directory tree is walked depth-first with normalized names", "[core][iso]") {
    Layout lay;
    const Image image = Image::from_bytes(sample_disc().build(&lay));
    const auto& entries = image.entries();
    REQUIRE_FALSE(entries.empty());
    CHECK(entries[0].path == "/");
    CHECK(entries[0].is_directory());
    CHECK(entries[0].depth == 0);

    const auto* noext = image.find("/NOEXT");
    REQUIRE(noext != nullptr);
    CHECK(noext->identifier == "NOEXT.;1");
    CHECK(noext->name == "NOEXT");
    CHECK(noext->size == 10);
    CHECK(noext->lba == lay.extent_lba.at("/NOEXT"));

    const auto* title = image.find("/BIN/TITLE.BIN");
    REQUIRE(title != nullptr);
    CHECK(title->identifier == "TITLE.BIN;1");
    CHECK(title->path == "/BIN/TITLE.BIN");
    CHECK(title->depth == 2);
    CHECK(entries[title->parent].path == "/BIN");

    const auto* ura = image.find("/MOVIE/EV/URA.MVI");
    REQUIRE(ura != nullptr);
    CHECK(ura->depth == 3);
    CHECK(entries[entries[ura->parent].parent].path == "/MOVIE");

    // Directories precede their contents (depth-first order).
    std::size_t bin = 0;
    std::size_t title_index = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].path == "/BIN") {
            bin = i;
        }
        if (entries[i].path == "/BIN/TITLE.BIN") {
            title_index = i;
        }
    }
    CHECK(bin < title_index);
    CHECK(image.find("/MODULES")->is_directory());
    CHECK(image.find("/EMPTY.TXT")->size == 0);
}

TEST_CASE("find is case-insensitive and tolerant of separators and versions", "[core][iso]") {
    const Image image = Image::from_bytes(sample_disc().build());
    const auto* title = image.find("/BIN/TITLE.BIN");
    REQUIRE(title != nullptr);
    CHECK(image.find("bin/title.bin") == title);
    CHECK(image.find("\\BIN\\TITLE.BIN;1") == title);
    CHECK(image.find("BIN//Title.Bin") == title);
    CHECK(image.find("noext.;1") == image.find("/NOEXT"));
    CHECK(image.find("NOEXT.") == image.find("/NOEXT"));
    CHECK(image.find("/") == &image.entries()[0]);
    CHECK(image.find("") == &image.entries()[0]);
    CHECK(image.find("/BIN/MISSING.BIN") == nullptr);
    CHECK(image.find("/BIN/TITLE") == nullptr);
    CHECK_THROWS_AS(image.require("nope"), Error);
    CHECK(Image::normalize_path("\\bin\\Title.bin;1") == "BIN/TITLE.BIN");
}

TEST_CASE("reads cross sector boundaries and are bounds-checked", "[core][iso]") {
    const Image image = Image::from_bytes(sample_disc().build());
    const auto* boot = image.find("BOOT.ELF");
    REQUIRE(boot != nullptr);
    const auto expected = seeded_bytes(2, 5000);

    const auto whole = image.read_all(*boot);
    CHECK(whole == expected);

    std::vector<std::uint8_t> window(100);
    image.read(*boot, 2000, window); // 2000..2100 straddles the first sector boundary
    CHECK(std::equal(window.begin(), window.end(), expected.begin() + 2000));

    std::vector<std::uint8_t> tail(1);
    image.read(*boot, 4999, tail);
    CHECK(tail[0] == expected[4999]);

    std::vector<std::uint8_t> empty;
    CHECK_NOTHROW(image.read(*boot, 5000, empty));
    CHECK_THROWS_AS(image.read(*boot, 5000, tail), Error);
    CHECK_THROWS_AS(image.read(*boot, 4901, window), Error);
    try {
        image.read(*boot, 4901, window);
    } catch (const Error& e) {
        CHECK(e.code() == ErrorCode::iso_range);
    }
    CHECK_THROWS_AS(image.read_all(*boot, 4999), Error);

    std::vector<std::uint8_t> sectors(2 * 2048);
    image.read_sectors(boot->lba, 2, sectors);
    CHECK(std::equal(sectors.begin(), sectors.end(), expected.begin()));
    CHECK_THROWS_AS(image.read_sectors(boot->lba, 3, sectors), Error);
    CHECK_THROWS_AS(
        image.read_sectors(image.image_sectors(), 1, std::span<std::uint8_t>(sectors.data(), 2048)), Error);
}

TEST_CASE("an image file on disk opens the same way", "[core][iso]") {
    dq8::test::TempDir tmp;
    const auto file = tmp.path() / "synthetic.iso";
    sample_disc().write(file);
    const Image image = Image::open(file);
    CHECK(image.find("/BIN/MENU.BIN")->size == 2049);
    CHECK(image.read_all(*image.find("/BIN/MENU.BIN")) == seeded_bytes(5, 2049));
    CHECK_THROWS_AS(Image::open(tmp.path() / "missing.iso"), Error);
}

TEST_CASE("a both-byte-order mismatch is reported as corruption", "[core][iso]") {
    Layout lay;
    auto bytes = sample_disc().build(&lay);
    // Flip the big-endian copy of TITLE.BIN's data length (record offset 14..18).
    bytes[lay.record_offset.at("/BIN/TITLE.BIN") + 17] ^= 0x01;
    CHECK(code_of(bytes) == ErrorCode::iso_endian_mismatch);

    auto bytes2 = sample_disc().build(&lay);
    bytes2[lay.pvd_offset + 84] ^= 0x01; // volume space size, big-endian half
    CHECK(code_of(bytes2) == ErrorCode::iso_endian_mismatch);
}

TEST_CASE("truncated images are refused", "[core][iso]") {
    Layout lay;
    auto bytes = sample_disc().build(&lay);
    const auto full = bytes;

    bytes.resize(16 * 2048); // ends before the primary descriptor
    CHECK(code_of(bytes) == ErrorCode::iso_descriptor);

    bytes = full;
    bytes.resize(17 * 2048); // descriptor present, terminator missing: the declared volume size gives it away
    CHECK(code_of(bytes) == ErrorCode::iso_truncated);

    bytes = full;
    bytes.resize(bytes.size() - 2048); // volume declares more sectors than the file holds
    CHECK(code_of(bytes) == ErrorCode::iso_truncated);

    // Same length, but the last file's extent points past the end.
    bytes = full;
    const auto off = lay.record_offset.at("/MOVIE/E904/A1.MVI");
    const std::uint32_t lba = static_cast<std::uint32_t>(bytes.size() / 2048);
    for (int i = 0; i < 4; ++i) {
        bytes[off + 2 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(lba >> (8 * i));
        bytes[off + 9 - static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(lba >> (8 * i));
    }
    CHECK(code_of(bytes) == ErrorCode::iso_truncated);
}

TEST_CASE("not an ISO image", "[core][iso]") {
    std::vector<std::uint8_t> junk(40 * 2048, 0x5A);
    CHECK(code_of(junk) == ErrorCode::iso_descriptor);
    std::vector<std::uint8_t> tiny(100, 0);
    CHECK(code_of(tiny) == ErrorCode::iso_descriptor);
}

TEST_CASE("a directory cycle is detected", "[core][iso]") {
    Layout lay;
    auto bytes = sample_disc().build(&lay);
    // Point MOVIE/EV's extent at MOVIE itself (both byte orders, size too), making EV -> MOVIE -> EV ...
    const auto off = lay.record_offset.at("/MOVIE/EV");
    const std::uint32_t movie_lba = lay.extent_lba.at("/MOVIE");
    const auto movie_off = lay.record_offset.at("/MOVIE");
    for (int i = 0; i < 4; ++i) {
        bytes[off + 2 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(movie_lba >> (8 * i));
        bytes[off + 9 - static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(movie_lba >> (8 * i));
        bytes[off + 10 + static_cast<std::size_t>(i)] = bytes[movie_off + 10 + static_cast<std::size_t>(i)];
        bytes[off + 17 - static_cast<std::size_t>(i)] = bytes[movie_off + 17 - static_cast<std::size_t>(i)];
    }
    const ErrorCode code = code_of(bytes);
    // The walker sees MOVIE's extent again through EV: either the cycle check or the self-entry check fires
    // first depending on record order; both mean "the tree is inconsistent".
    CHECK((code == ErrorCode::iso_cycle || code == ErrorCode::iso_record));
}

TEST_CASE("a directory that points at an already visited extent is a cycle", "[core][iso]") {
    Layout lay;
    auto bytes = sample_disc().build(&lay);
    // Make BIN's record point at the root extent. The root's self entry then matches (its own LBA), so the
    // only thing that can catch it is the visited set.
    const auto off = lay.record_offset.at("/BIN");
    const std::uint32_t root_lba = lay.root_lba;
    const auto root_off = lay.record_offset.at("/");
    for (int i = 0; i < 4; ++i) {
        bytes[off + 2 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(root_lba >> (8 * i));
        bytes[off + 9 - static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(root_lba >> (8 * i));
        bytes[off + 10 + static_cast<std::size_t>(i)] = bytes[root_off + 10 + static_cast<std::size_t>(i)];
        bytes[off + 17 - static_cast<std::size_t>(i)] = bytes[root_off + 17 - static_cast<std::size_t>(i)];
    }
    CHECK(code_of(bytes) == ErrorCode::iso_cycle);
}

TEST_CASE("multi-extent and interleaved files are refused", "[core][iso]") {
    Layout lay;
    auto bytes = sample_disc().build(&lay);
    bytes[lay.record_offset.at("/BOOT.ELF") + 25] |= 0x80; // multi-extent flag
    CHECK(code_of(bytes) == ErrorCode::iso_unsupported);

    bytes = sample_disc().build(&lay);
    bytes[lay.record_offset.at("/BOOT.ELF") + 26] = 1; // file unit size: interleaved
    CHECK(code_of(bytes) == ErrorCode::iso_unsupported);

    bytes = sample_disc().build(&lay);
    bytes[lay.record_offset.at("/BOOT.ELF") + 1] = 1; // extended attribute record
    CHECK(code_of(bytes) == ErrorCode::iso_unsupported);
}

TEST_CASE("malformed directory records are refused", "[core][iso]") {
    Layout lay;
    auto bytes = sample_disc().build(&lay);
    bytes[lay.record_offset.at("/BOOT.ELF")] = 20; // shorter than the fixed part
    CHECK(code_of(bytes) == ErrorCode::iso_record);

    bytes = sample_disc().build(&lay);
    bytes[lay.record_offset.at("/BOOT.ELF") + 32] = 200; // name length beyond the record
    CHECK(code_of(bytes) == ErrorCode::iso_record);
}

TEST_CASE("the path table must agree with the directory tree", "[core][iso]") {
    Layout lay;
    auto bytes = sample_disc().build(&lay);
    // Shrink the recorded path table size so a directory goes missing from it.
    const std::uint32_t smaller = lay.path_table_size - 10;
    for (int i = 0; i < 4; ++i) {
        bytes[lay.pvd_offset + 132 + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(smaller >> (8 * i));
        bytes[lay.pvd_offset + 139 - static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(smaller >> (8 * i));
    }
    CHECK(code_of(bytes) == ErrorCode::iso_path_table);

    // Corrupt one record's extent in the M table only: L and M disagree.
    bytes = sample_disc().build(&lay);
    bytes[static_cast<std::size_t>(lay.path_table_m_lba) * 2048 + 10 + 5] ^= 0x01;
    CHECK(code_of(bytes) == ErrorCode::iso_path_table);
}

TEST_CASE("unsupported block size is refused", "[core][iso]") {
    Layout lay;
    auto bytes = sample_disc().build(&lay);
    bytes[lay.pvd_offset + 128] = 0x00;
    bytes[lay.pvd_offset + 129] = 0x04; // 1024, both halves
    bytes[lay.pvd_offset + 130] = 0x04;
    bytes[lay.pvd_offset + 131] = 0x00;
    CHECK(code_of(bytes) == ErrorCode::iso_unsupported);
}

TEST_CASE("a padded image (volume smaller than the file) is accepted", "[core][iso]") {
    IsoBuilder b = sample_disc();
    Layout lay;
    auto bytes = b.build(&lay);
    bytes.resize(bytes.size() + 7 * 2048, 0);
    const Image image = Image::from_bytes(bytes);
    CHECK(image.image_sectors() == image.volume_sectors() + 7);
}
