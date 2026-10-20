// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/error.h"
#include "dq8/core/mwo3.h"
#include "support/mwo3_builder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using dq8::core::Error;
using dq8::core::ErrorCode;
using dq8::core::Mwo3Window;
using dq8::test::build_mwo3;
using dq8::test::Mwo3Spec;
using dq8::test::put_le32;

namespace {

const Mwo3Window window{0x00200000, 0x00210000};

// Parses `file` against `window` and returns the error code, or fails the test when it parses.
ErrorCode code_of(const std::vector<std::uint8_t>& file, std::optional<Mwo3Window> w = window,
                  const std::string& fragment = "") {
    try {
        (void)dq8::core::parse_mwo3(file, w);
    } catch (const Error& e) {
        if (!fragment.empty()) {
            CHECK_THAT(e.what(), ContainsSubstring(fragment));
        }
        return e.code();
    }
    FAIL("expected the container to be refused");
    return ErrorCode::io_open;
}

} // namespace

TEST_CASE("a valid MWo3 container parses with its fields and derived addresses", "[core][mwo3]") {
    Mwo3Spec spec;
    spec.overlay_id = 3;
    spec.name = "unit-test";
    const auto file = build_mwo3(spec);
    REQUIRE(file.size() == 64 + 0x200 + 0x80);
    CHECK(dq8::core::looks_like_mwo3(file));

    const auto o = dq8::core::parse_mwo3(file, window);
    const auto& h = o.header;
    CHECK(h.magic == dq8::core::mwo3_magic_word);
    CHECK(h.overlay_id == 3);
    CHECK(h.load_address == 0x00200000);
    CHECK(h.text_size == 0x200);
    CHECK(h.data_size == 0x80);
    CHECK(h.bss_size == 0x40);
    CHECK(h.table_start == 0x00200000 + 64 + 0x200 + 0x10);
    CHECK(h.table_end == h.table_start + 8);
    CHECK(h.name == "unit-test");

    CHECK(h.file_size() == 64 + 0x200 + 0x80);
    CHECK(h.memsz() == 64 + 0x200 + 0x80 + 0x40);
    CHECK(h.code_start() == 0x00200040);
    CHECK(h.data_start() == 0x00200240);
    CHECK(h.bss_start() == 0x002002C0);
    CHECK(h.end_address() == 0x00200300);
    CHECK(h.table_entry_count() == 2);

    CHECK(h.raw_words[0] == dq8::core::mwo3_magic_word);
    CHECK(h.raw_words[1] == 3);
    CHECK(h.raw_words[5] == 0x40);
    CHECK(h.raw_words[7] == h.table_end);
    CHECK(h.raw_words[8] == 0x74696E75u); // "unit" as a little-endian word
    CHECK(h.raw_words[15] == 0);

    // image() is the file minus the header, text and data together.
    REQUIRE(o.image.size() == 0x280);
    CHECK(std::memcmp(o.image.data(), file.data() + 64, o.image.size()) == 0);

    // Header-only parsing gives the same header; without a window the overlay's own range is used.
    const auto head_only = dq8::core::parse_mwo3_header(file, file.size());
    CHECK(head_only.table_end == h.table_end);
    CHECK(head_only.name == h.name);
    CHECK(dq8::core::parse_mwo3(file).header.memsz() == h.memsz());
}

TEST_CASE("empty tables and empty data ranges are accepted", "[core][mwo3]") {
    Mwo3Spec spec;
    spec.table_entries = 0;
    spec.table_offset = spec.data_size; // an empty table at the very end of the data range
    CHECK(dq8::core::parse_mwo3(build_mwo3(spec), window).header.table_entry_count() == 0);

    spec.data_size = 0;
    spec.table_offset = 0;
    CHECK(dq8::core::parse_mwo3(build_mwo3(spec), window).header.data_start() ==
          dq8::core::parse_mwo3(build_mwo3(spec), window).header.table_start);

    spec.bss_size = 0;
    CHECK(dq8::core::parse_mwo3(build_mwo3(spec), window).header.end_address() == 0x00200000 + 64 + 0x200);
}

TEST_CASE("every MWo3 validation failure has its own code", "[core][mwo3]") {
    const Mwo3Spec spec;
    const auto good = build_mwo3(spec);

    SECTION("magic") {
        auto bad = good;
        bad.resize(40);
        CHECK(code_of(bad, window, "shorter than an MWo3 header") == ErrorCode::mwo3_magic);
        bad = good;
        put_le32(bad, 0, 0x336F574Eu);
        CHECK(code_of(bad, window, "not an MWo3") == ErrorCode::mwo3_magic);
        CHECK_FALSE(dq8::core::looks_like_mwo3(bad));
        CHECK_FALSE(dq8::core::looks_like_mwo3(std::vector<std::uint8_t>{'M', 'W'}));
    }
    SECTION("size") {
        auto bad = good;
        bad.push_back(0);
        CHECK(code_of(bad, window, "header declares") == ErrorCode::mwo3_size);
        bad = good;
        bad.pop_back();
        CHECK(code_of(bad) == ErrorCode::mwo3_size);
        bad = good;
        put_le32(bad, 0x0C, 0x1FC); // text_size off by four
        CHECK(code_of(bad) == ErrorCode::mwo3_size);
        bad = good;
        put_le32(bad, 0x10, 0x84); // data_size off by four
        CHECK(code_of(bad) == ErrorCode::mwo3_size);
    }
    SECTION("overlay id") {
        auto bad = good;
        put_le32(bad, 0x04, 0);
        CHECK(code_of(bad, window, "overlay_id 0") == ErrorCode::mwo3_id);
        put_le32(bad, 0x04, dq8::core::mwo3_max_overlay_id + 1);
        CHECK(code_of(bad) == ErrorCode::mwo3_id);
        put_le32(bad, 0x04, dq8::core::mwo3_max_overlay_id);
        CHECK_NOTHROW(dq8::core::parse_mwo3(bad, window));
    }
    SECTION("window") {
        // Not at the window base; a window too small; bss beyond the window; past the 4 GiB boundary.
        CHECK(code_of(good, Mwo3Window{0x00200040, 0x00210000}, "not the overlay window base") ==
              ErrorCode::mwo3_range);
        CHECK(code_of(good, Mwo3Window{0x00200000, 0x002002FF}, "holds 767") == ErrorCode::mwo3_range);
        CHECK_NOTHROW(dq8::core::parse_mwo3(good, Mwo3Window{0x00200000, 0x00200300}));
        auto bad = good;
        put_le32(bad, 0x14, 0x10000); // bss_size
        CHECK(code_of(bad) == ErrorCode::mwo3_range);
        Mwo3Spec far = spec;
        far.load_address = 0xFFFFFF00u;
        far.bss_size = 0x2000;
        CHECK(code_of(build_mwo3(far), std::nullopt, "4 GiB") == ErrorCode::mwo3_range);
        far.bss_size = 0;
        CHECK(code_of(build_mwo3(far), std::nullopt, "4 GiB") == ErrorCode::mwo3_range);
    }
    SECTION("constructor table") {
        auto bad = good;
        put_le32(bad, 0x18, spec.table_start() + 1);
        CHECK(code_of(bad, window, "aligned") == ErrorCode::mwo3_table);
        bad = good;
        put_le32(bad, 0x1C, spec.table_end() + 2);
        CHECK(code_of(bad, window, "aligned") == ErrorCode::mwo3_table);
        bad = good;
        put_le32(bad, 0x1C, spec.table_start() - 4);
        CHECK(code_of(bad, window, "before it starts") == ErrorCode::mwo3_table);
        bad = good;
        put_le32(bad, 0x18, spec.data_start() - 4); // starts in the text range
        CHECK(code_of(bad, window, "outside the data range") == ErrorCode::mwo3_table);
        bad = good;
        put_le32(bad, 0x18, spec.data_start() + spec.data_size);
        put_le32(bad, 0x1C, spec.data_start() + spec.data_size + 4); // ends in bss
        CHECK(code_of(bad, window, "outside the data range") == ErrorCode::mwo3_table);
        bad = good;
        put_le32(bad, 0x18, 0); // nowhere near the overlay
        put_le32(bad, 0x1C, 0);
        CHECK(code_of(bad) == ErrorCode::mwo3_table);
    }
    SECTION("name") {
        auto bad = good;
        for (std::size_t i = 0x20; i < 0x40; ++i) {
            bad.at(i) = 'a'; // no terminator
        }
        CHECK(code_of(bad, window, "NUL-terminated") == ErrorCode::mwo3_name);
        bad = good;
        bad.at(0x20) = 0; // empty
        CHECK(code_of(bad, window, "empty") == ErrorCode::mwo3_name);
        bad = good;
        bad.at(0x21) = 0x01; // control character inside the name
        CHECK(code_of(bad, window, "non-printable byte 0x01 at offset 1") == ErrorCode::mwo3_name);
        bad = good;
        bad.at(0x22) = 0xC3; // not ASCII
        CHECK(code_of(bad, window, "non-printable") == ErrorCode::mwo3_name);
        bad = good;
        bad.at(0x3F) = 'x'; // a byte after the terminator
        CHECK(code_of(bad, window, "NUL-padded") == ErrorCode::mwo3_name);
    }
}

TEST_CASE("the mwo3 error codes are not image mismatches and have stable names", "[core][mwo3]") {
    CHECK_FALSE(dq8::core::is_mismatch(ErrorCode::mwo3_magic));
    CHECK_FALSE(dq8::core::is_mismatch(ErrorCode::mwo3_table));
    CHECK(dq8::core::to_string(ErrorCode::mwo3_magic) == "mwo3-magic");
    CHECK(dq8::core::to_string(ErrorCode::mwo3_size) == "mwo3-size");
    CHECK(dq8::core::to_string(ErrorCode::mwo3_id) == "mwo3-id");
    CHECK(dq8::core::to_string(ErrorCode::mwo3_range) == "mwo3-range");
    CHECK(dq8::core::to_string(ErrorCode::mwo3_table) == "mwo3-table");
    CHECK(dq8::core::to_string(ErrorCode::mwo3_name) == "mwo3-name");
}
