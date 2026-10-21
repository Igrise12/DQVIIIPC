// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/error.h"
#include "dq8/core/region_profile.h"
#include "support/iso_builder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
code_offset = 64
window_pointer = 0x00100100
loader = 0x00100200
loader_hook = 0x00100210
load_by_index = 0x00100300
name_table = 0x00100400
name_table_order = ["A"]
lsn_cache = 0x00100500
ctor_walker = 0x00100600
flush_cache_stub = 0x00100700
mode_variable = 0x00100800
[[overlays]]
name = "A"
id = 1
index = 0
segment_index = 1
path = "BIN/A.BIN"
segment_memsz = 0x300
text_size = 0x200
data_size = 0x80
bss_size = 0x40
table_entries = 2
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
    CHECK(p.overlay_window.code_offset == 64);
    CHECK(p.overlay_window.loader == 0x00100200);
    CHECK(p.overlay_window.mode_variable == 0x00100800);
    REQUIRE(p.overlay_window.name_table_order.size() == 1);
    CHECK(p.overlay_window.name_table_order[0] == "A");
    REQUIRE(p.overlays.size() == 1);
    CHECK(p.overlays[0].segment_memsz == 0x300);
    CHECK(p.overlays[0].index == 0);
    CHECK(p.overlays[0].segment_index == 1);
    CHECK(p.overlays[0].text_size == 0x200);
    CHECK(p.overlays[0].data_size == 0x80);
    CHECK(p.overlays[0].bss_size == 0x40);
    CHECK(p.overlays[0].table_entries == 2);
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

TEST_CASE("the overlay window and overlay rows are cross-validated", "[core][profile]") {
    dq8::test::TempDir tmp;
    auto expect = [&](const std::string& from, const std::string& to, const std::string& fragment) {
        const auto file = write(tmp.path(), "bad.toml", replace_line(minimal_profile, from, to));
        try {
            (void)dq8::core::load_region_profile(file);
        } catch (const Error& e) {
            CHECK(e.code() == ErrorCode::profile_schema);
            CHECK_THAT(e.what(), ContainsSubstring(fragment));
            CHECK_THAT(e.what(), ContainsSubstring("bad.toml:"));
            return;
        }
        FAIL("expected an error containing: " << fragment);
    };
    expect("header_size = 64", "header_size = 32", "header_size must be 64 for an MWo3 container");
    expect("code_offset = 64", "code_offset = 32", "code_offset must equal header_size");
    expect("loader = 0x00100200", "loader = 0x00100201", "loader must be 4-byte aligned");
    expect("loader = 0x00100200", "loader = 0x00200000", "loader must lie inside the main program");
    expect("mode_variable = 0x00100800", "mode_variable = 0x000FFFFC", "inside the main program");
    expect("window_pointer = 0x00100100\n", "", "missing 'window_pointer'");
    expect("name_table_order = [\"A\"]", "name_table_order = [\"B\"]", "index 0 is not the position of 'A'");
    expect("name_table_order = [\"A\"]", "name_table_order = [\"A\", \"A\"]", "lists 'A' twice");
    expect("name_table_order = [\"A\"]", "name_table_order = [\"A\", \"B\"]",
           "has 2 entries but there are 1 [[overlays]]");
    expect("name_table_order = [\"A\"]", "name_table_order = \"A\"", "must be an array of strings");
    expect("\nindex = 0\n", "\nindex = 1\n", "index 1 is not the position of 'A'");
    expect("segment_index = 1", "segment_index = 0", "segment_index must be unique and at least 1");
    expect("id = 1\nindex", "id = 65\nindex", "between 1 and 64");
    expect("bss_size = 0x40", "bss_size = 0x44", "must equal segment_memsz");
    expect("text_size = 0x200", "text_size = 0x1FC", "must equal segment_memsz");
    expect("size = 0\n", "size = 100\n", "size must equal header_size + text_size + data_size");
    expect("table_entries = 2", "table_entries = 33", "table_entries do not fit in data_size");
    expect("table_entries = 2\n", "", "missing 'table_entries'");

    // Two rows sharing an index, id, name or segment.
    const std::string second = "[[overlays]]\nname = \"B\"\nid = 2\nindex = 1\nsegment_index = 2\n"
                               "path = \"BIN/B.BIN\"\nsegment_memsz = 0x80\ntext_size = 0x20\n"
                               "data_size = 0x10\nbss_size = 0x10\ntable_entries = 2\nsize = 0\n";
    auto with_second = [&](const std::string& from, const std::string& to) {
        std::string text =
            replace_line(minimal_profile, "name_table_order = [\"A\"]", "name_table_order = [\"A\", \"B\"]");
        text = replace_line(text, "[[files]]", second + "[[files]]");
        return replace_line(text, from, to);
    };
    CHECK_NOTHROW(
        dq8::core::load_region_profile(write(tmp.path(), "two.toml", with_second("size = 0", "size = 0"))));
    auto expect_second = [&](const std::string& from, const std::string& to, const std::string& fragment) {
        const auto file = write(tmp.path(), "bad2.toml", with_second(from, to));
        CHECK_THROWS_AS(dq8::core::load_region_profile(file), Error);
        try {
            (void)dq8::core::load_region_profile(file);
        } catch (const Error& e) {
            CHECK_THAT(e.what(), ContainsSubstring(fragment));
        }
    };
    expect_second("id = 2\nindex = 1", "id = 1\nindex = 1", "id must be unique");
    expect_second("segment_index = 2", "segment_index = 1", "segment_index must be unique");
    expect_second("name = \"B\"\nid = 2", "name = \"A\"\nid = 2", "name 'A' is used twice");
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
            CHECK(p.overlay_window.end == 0x00489A80);
            CHECK(p.overlay_window.header_size == 64);
            CHECK(p.overlay_window.code_offset == 0x40);
            CHECK(p.overlay_window.window_pointer == 0x003D1704);
            CHECK(p.overlay_window.loader == 0x00100330);
            CHECK(p.overlay_window.loader_hook == 0x00100320);
            CHECK(p.overlay_window.load_by_index == 0x00169860);
            CHECK(p.overlay_window.name_table == 0x003950F0);
            CHECK(p.overlay_window.lsn_cache == 0x003F8850);
            CHECK(p.overlay_window.ctor_walker == 0x00100DE0);
            CHECK(p.overlay_window.flush_cache_stub == 0x00116AA0);
            CHECK(p.overlay_window.mode_variable == 0x003D2740);
            REQUIRE(p.overlay_window.name_table_order.size() == 6);
            CHECK(p.overlay_window.name_table_order[0] == "TITLE");
            CHECK(p.overlay_window.name_table_order[5] == "SHOP");
            for (const auto& o : p.overlays) {
                CHECK(p.overlay_window.name_table_order.at(o.index) == o.name);
                CHECK(o.segment_index == o.id); // the segments are in id order on this disc
                CHECK(64 + o.text_size + o.data_size + o.bss_size == o.segment_memsz);
                CHECK(64 + o.text_size + o.data_size == o.size);
                if (o.name == "TITLE") {
                    CHECK(o.index == 0);
                    CHECK(o.table_entries == 6);
                    CHECK(o.bss_size == 0x600);
                }
            }
            CHECK(p.boot.sha1 == "f24bfd21c21f6454c05934224ab52cde837bc90b");
            CHECK(p.title_screen.overlay == "TITLE");
            CHECK(p.title_screen.variable == 0x003D4008);
            CHECK(p.title_screen.width == 1);
            CHECK(p.title_screen.value == 1);
            CHECK(p.movie.finished_query == 0x001A1380);
            CHECK(p.boot.ctor_table_start == 0x003CF910);
            CHECK(p.boot.ctor_table_end == 0x003CFB80);
            CHECK(p.iop.reset_image == "MODULES/IOPRP300.IMG");
            REQUIRE(p.iop.modules.size() == 15); // S7's load order
            CHECK(p.iop.modules.front().name == "SIO2MAN");
            CHECK(p.iop.modules.back().name == "PCMPLAY");
            CHECK(p.iop.modules.back().services == std::vector<std::string>{"null:0x123456"});
            REQUIRE(p.movie.player_entries.size() == 2);
            CHECK(p.movie.scene_unit == "TITLE");
            CHECK(p.movie.scene_functions.size() == 3);
        }
    }
    CHECK(found);
}

TEST_CASE("the optional [iop] and [movie] tables are validated", "[core][profile]") {
    dq8::test::TempDir tmp;
    const std::string base = std::string(minimal_profile) +
                             "[[files]]\npath = \"MODULES/IOPRP.IMG\"\nrole = \"iop_image\"\nsize = 0\n";
    const std::string iop =
        "[iop]\nreset_image = \"MODULES/IOPRP.IMG\"\n"
        "[[iop.modules]]\nname = \"X\"\nservices = [\"padman.v1\", \"null:0x80000701\"]\n";
    const std::string movie = "[movie]\nplayer_entries = [0x00100020]\nscene_unit = \"A\"\n"
                              "scene_functions = [0x00200040]\n";

    // Absent tables leave the fields empty.
    const auto plain = dq8::core::load_region_profile(write(tmp.path(), "plain.toml", minimal_profile));
    CHECK(plain.iop.modules.empty());
    CHECK(plain.movie.player_entries.empty());

    const auto good = dq8::core::load_region_profile(write(tmp.path(), "good.toml", base + iop + movie));
    REQUIRE(good.iop.modules.size() == 1);
    CHECK(good.iop.modules[0].services.size() == 2);
    CHECK(good.movie.scene_functions == std::vector<std::uint32_t>{0x00200040});

    auto fails = [&](const std::string& text, const std::string& what) {
        const auto file = write(tmp.path(), "bad.toml", text);
        CHECK_THROWS_WITH(dq8::core::load_region_profile(file), ContainsSubstring(what));
    };
    fails(base + replace_line(iop, "IOPRP.IMG", "X.IRX") + movie, "role iop_image");
    fails(base + replace_line(iop, "name = \"X\"", "name = \"Y\"") + movie, "no [[files]] entry for Y.IRX");
    fails(base + replace_line(iop, "name = \"X\"", "name = \"x\"") + movie, "upper-case");
    fails(base + replace_line(iop, "padman.v1", "PadMan") + movie, "catalogue names");
    fails(base + replace_line(iop, "null:0x80000701", "null:12") + movie, "catalogue names");
    fails(base + iop + replace_line(movie, "0x00100020", "0x00300000"), "inside the main program");
    fails(base + iop + replace_line(movie, "0x00200040", "0x00100040"), "inside the overlay window");
    fails(base + iop + replace_line(movie, "scene_unit = \"A\"", "scene_unit = \"B\""),
          "not an overlay name");
    fails(base + iop + replace_line(movie, "0x00100020", "0x00100022"), "4-byte aligned");

    const std::string with_ctor = replace_line(minimal_profile, "main_memsz = 0x1000",
                                               "main_memsz = 0x1000\nctor_table = [0x00100F00, 0x00100F10]");
    const auto ctor = dq8::core::load_region_profile(write(tmp.path(), "ctor.toml", with_ctor));
    CHECK(ctor.boot.ctor_table_start == 0x00100F00);
    CHECK(ctor.boot.ctor_table_end == 0x00100F10);
    CHECK(plain.boot.ctor_table_start == 0);
    fails(replace_line(minimal_profile, "main_memsz = 0x1000",
                       "main_memsz = 0x1000\nctor_table = [0x00100F00, 0x00200000]"),
          "inside the main program");
    fails(replace_line(minimal_profile, "main_memsz = 0x1000",
                       "main_memsz = 0x1000\nctor_table = [0x00100F00]"),
          "[start, end]");

    const std::string title = "[title]\noverlay = \"A\"\nvariable = 0x00100F00\nwidth = 2\nvalue = 1000\n";
    const auto titled =
        dq8::core::load_region_profile(write(tmp.path(), "title.toml", std::string(minimal_profile) + title));
    CHECK(titled.title_screen.overlay == "A");
    CHECK(titled.title_screen.value == 1000);
    CHECK(plain.title_screen.variable == 0);
    fails(std::string(minimal_profile) + replace_line(title, "overlay = \"A\"", "overlay = \"B\""),
          "not an overlay name");
    fails(std::string(minimal_profile) + replace_line(title, "width = 2", "width = 3"), "1, 2 or 4");
    fails(std::string(minimal_profile) + replace_line(title, "0x00100F00", "0x00100F01"),
          "aligned to its width");
    fails(base + iop + movie + "finished_query = 0x00300000\n", "finished_query");
}
