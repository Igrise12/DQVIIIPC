// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/error.h"
#include "dq8/core/region_profile.h"
#include "support/iso_builder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <fstream>
#include <string>

using Catch::Matchers::ContainsSubstring;
using dq8::core::Error;
using dq8::core::ErrorCode;

namespace {

const char* const minimal_profile = R"(schema_version = 1
[profile]
id = "TEST_00001"
serial = "TEST-00001"
title = "Synthetic"
region = "NTSC-U"
video_mode = "NTSC"
[image]
size = 1048576
sector_size = 2048
[boot]
path = "BOOT.ELF"
size = 1234
sha1 = "da39a3ee5e6b4b0d3255bfef95601890afd80709"
crc32 = "00000000"
entry = 0x00100008
load_address = 0x00100000
main_memsz = 0x1000
[overlay_window]
base = 0x00200000
end = 0x00210000
container = "MWo3"
header_size = 64
[[overlays]]
name = "A"
id = 1
path = "BIN/A.BIN"
segment_memsz = 0x300
size = 0
[[files]]
path = "MODULES/X.IRX"
role = "iop_module"
size = 0
sha1 = ""
crc32 = ""
[not_extracted]
paths = ["DATA.DAT", "MOVIE/"]
)";

std::filesystem::path write(const std::filesystem::path& dir, const std::string& name,
                            const std::string& text) {
    const auto file = dir / name;
    std::ofstream(file, std::ios::binary) << text;
    return file;
}

std::string replace_line(std::string text, const std::string& from, const std::string& to) {
    const auto pos = text.find(from);
    REQUIRE(pos != std::string::npos);
    return text.replace(pos, from.size(), to);
}

} // namespace

TEST_CASE("a well-formed profile loads", "[core][profile]") {
    dq8::test::TempDir tmp;
    const auto file = write(tmp.path(), "TEST_00001.toml", minimal_profile);
    const auto p = dq8::core::load_region_profile(file);
    CHECK(p.id == "TEST_00001");
    CHECK(p.boot.path == "BOOT.ELF");
    CHECK(p.boot.entry == 0x00100008);
    CHECK(p.boot.sha1 == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(p.overlay_window.base == 0x00200000);
    REQUIRE(p.overlays.size() == 1);
    CHECK(p.overlays[0].segment_memsz == 0x300);
    CHECK(p.overlays[0].sha1.empty());
    REQUIRE(p.files.size() == 1);
    CHECK(p.files[0].role == "iop_module");
    REQUIRE(p.not_extracted.size() == 2);
    CHECK(p.not_extracted[1] == "MOVIE/");
    CHECK(p.source == file);

    const auto all = dq8::core::load_region_profiles(tmp.path());
    REQUIRE(all.size() == 1);
    CHECK(dq8::core::find_profile_by_boot_sha1(all, "DA39A3EE5E6B4B0D3255BFEF95601890AFD80709") == &all[0]);
    CHECK(dq8::core::find_profile_by_boot_sha1(all, "0000000000000000000000000000000000000000") == nullptr);
}

TEST_CASE("schema violations are reported with the file and line", "[core][profile]") {
    dq8::test::TempDir tmp;
    auto expect = [&](const std::string& text, ErrorCode code, const std::string& fragment) {
        const auto file = write(tmp.path(), "bad.toml", text);
        try {
            (void)dq8::core::load_region_profile(file);
        } catch (const Error& e) {
            CHECK(e.code() == code);
            CHECK_THAT(e.what(), ContainsSubstring(fragment));
            CHECK_THAT(e.what(), ContainsSubstring("bad.toml:"));
            return;
        }
        FAIL("expected an error containing: " << fragment);
    };
    expect(replace_line(minimal_profile, "sha1 = \"da39a3ee5e6b4b0d3255bfef95601890afd80709\"\n", ""),
           ErrorCode::profile_schema, "[boot] is missing 'sha1'");
    expect(replace_line(minimal_profile, "sha1 = \"da39a3ee5e6b4b0d3255bfef95601890afd80709\"",
                        "sha1 = \"abc\""),
           ErrorCode::profile_schema, "40 hex digits");
    expect(replace_line(minimal_profile, "entry = 0x00100008", "entry = \"0x00100008\""),
           ErrorCode::profile_schema, "must be an integer");
    expect(replace_line(minimal_profile, "schema_version = 1", "schema_version = 2"),
           ErrorCode::profile_schema, "schema_version 2");
    expect(replace_line(minimal_profile, "sector_size = 2048", "sector_size = 2352"),
           ErrorCode::profile_schema, "sector_size must be 2048");
    expect(replace_line(minimal_profile, "role = \"iop_module\"", "role = \"wat\""),
           ErrorCode::profile_schema, "role 'wat'");
    expect(replace_line(minimal_profile, "path = \"MODULES/X.IRX\"", "path = \"../X.IRX\""),
           ErrorCode::profile_schema, "not a plain relative disc path");
    expect(replace_line(minimal_profile, "path = \"MODULES/X.IRX\"", "path = \"MOVIE/X.IRX\""),
           ErrorCode::profile_schema, "[not_extracted] excludes");
    expect(replace_line(minimal_profile, "path = \"MODULES/X.IRX\"", "path = \"BIN/A.BIN\""),
           ErrorCode::profile_schema, "twice");
    expect(replace_line(minimal_profile, "[overlay_window]\n", ""), ErrorCode::profile_schema,
           "missing table [overlay_window]");
    expect("schema_version = 1\n[profile\nid = 1\n", ErrorCode::profile_parse, "");
}

TEST_CASE("conflicting profiles are refused", "[core][profile]") {
    dq8::test::TempDir tmp;
    write(tmp.path(), "a.toml", minimal_profile);
    write(tmp.path(), "b.toml", minimal_profile);
    CHECK_THROWS_AS(dq8::core::load_region_profiles(tmp.path()), Error);
    try {
        (void)dq8::core::load_region_profiles(tmp.path());
    } catch (const Error& e) {
        CHECK(e.code() == ErrorCode::profile_conflict);
    }
    CHECK_THROWS_AS(dq8::core::load_region_profiles(tmp.path() / "missing"), Error);
}

TEST_CASE("the shipped region profiles are valid", "[core][profile]") {
    const std::filesystem::path dir = std::filesystem::path(DQ8_SOURCE_DIR) / "data" / "regions";
    const auto profiles = dq8::core::load_region_profiles(dir);
    REQUIRE_FALSE(profiles.empty());
    bool found = false;
    for (const auto& p : profiles) {
        if (p.id == "SLUS_21207") {
            found = true;
            CHECK(p.overlays.size() == 6);
            CHECK(p.overlay_window.base == 0x00461C80);
            CHECK(p.boot.sha1 == "f24bfd21c21f6454c05934224ab52cde837bc90b");
        }
    }
    CHECK(found);
}
