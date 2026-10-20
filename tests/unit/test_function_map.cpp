// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/error.h"
#include "dq8/core/recomp/function_map.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <string>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using dq8::core::Error;
using dq8::core::ErrorCode;
namespace recomp = dq8::core::recomp;
namespace fs = std::filesystem;

// Synthetic maps: our own names and addresses in a made-up code range [0x00200000, 0x00201000).
namespace {

constexpr std::uint32_t code_start = 0x00200000;
constexpr std::uint32_t code_end = 0x00201000;

std::string csv(const std::string& rows) {
    return "Name,Start,End,Size\n" + rows;
}

// Runs `fn` and returns the Error it throws, failing the test when it does not throw.
template <typename Fn> Error error_of(Fn&& fn) {
    try {
        fn();
    } catch (const Error& e) {
        return e;
    }
    FAIL("expected an Error");
    return Error(ErrorCode::io_open, "");
}

recomp::FunctionMap map_of(const std::string& rows) {
    recomp::FunctionMap map;
    map.unit = "unit";
    map.functions = recomp::parse_function_csv(csv(rows), "unit.csv");
    return map;
}

} // namespace

TEST_CASE("parse_function_csv reads rows with hex and decimal addresses", "[core][recomp][map]") {
    const auto rows = recomp::parse_function_csv(csv("entry,0x00200000,0x00200010,16\r\n"
                                                     "\n"
                                                     "helper_one,2097168,0x00200020,16\n"
                                                     "FUN_00200020,0x00200020,0x00200100,224\n"),
                                                 "unit.csv");
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].name == "entry");
    CHECK(rows[0].start == 0x00200000);
    CHECK(rows[0].end == 0x00200010);
    CHECK(rows[0].line == 2);
    CHECK(rows[1].start == 0x00200010);
    CHECK(rows[1].line == 4);
    CHECK(rows[2].name == "FUN_00200020");
}

TEST_CASE("parse_function_csv refuses malformed rows with file and line", "[core][recomp][map]") {
    auto parse_error = [](const std::string& text) {
        return error_of([&] { (void)recomp::parse_function_csv(text, "unit.csv"); });
    };
    SECTION("a missing header") {
        const Error e = parse_error("entry,0x00200000,0x00200010,16\n");
        CHECK(e.code() == ErrorCode::map_parse);
        CHECK_THAT(e.what(), StartsWith("unit.csv:1:") && ContainsSubstring("header"));
    }
    SECTION("an empty file") {
        CHECK(parse_error("").code() == ErrorCode::map_parse);
    }
    SECTION("the wrong number of fields") {
        const Error e = parse_error(csv("a,0x00200000,0x00200010\n"));
        CHECK_THAT(e.what(), StartsWith("unit.csv:2:") && ContainsSubstring("4 fields"));
    }
    SECTION("an unparsable number") {
        CHECK_THAT(parse_error(csv("a,0x0020000G,0x00200010,16\n")).what(),
                   ContainsSubstring("hex or decimal"));
        CHECK_THAT(parse_error(csv("a,-4,0x00200010,16\n")).what(), ContainsSubstring("hex or decimal"));
        CHECK_THAT(parse_error(csv("a,0x100000000,0x00200010,16\n")).what(),
                   ContainsSubstring("hex or decimal"));
    }
    SECTION("an inverted or empty range") {
        CHECK_THAT(parse_error(csv("a,0x00200010,0x00200010,0\n")).what(), ContainsSubstring("exclusive"));
    }
    SECTION("a size that disagrees with the range") {
        CHECK_THAT(parse_error(csv("a,0x00200000,0x00200010,12\n")).what(), ContainsSubstring("End - Start"));
    }
    SECTION("misaligned addresses") {
        CHECK_THAT(parse_error(csv("a,0x00200002,0x00200010,14\n")).what(),
                   ContainsSubstring("multiples of 4"));
    }
    SECTION("bad names") {
        CHECK_THAT(parse_error(csv(",0x00200000,0x00200010,16\n")).what(), ContainsSubstring("empty"));
        CHECK_THAT(parse_error(csv(" a,0x00200000,0x00200010,16\n")).what(), ContainsSubstring("spaces"));
        CHECK_THAT(parse_error(csv("a\"b,0x00200000,0x00200010,16\n")).what(),
                   ContainsSubstring("printable"));
    }
}

TEST_CASE("validate_function_map sorts and checks functions against the unit range", "[core][recomp][map]") {
    SECTION("a good map is sorted") {
        auto map = map_of("b,0x00200010,0x00200020,16\na,0x00200000,0x00200010,16\n");
        CHECK(recomp::validate_function_map(map, code_start, code_end).empty());
        CHECK(map.functions[0].name == "a");
        CHECK(map.functions[1].name == "b");
    }
    SECTION("outside the unit") {
        auto map = map_of("a,0x00201000,0x00201010,16\n");
        const Error e = error_of([&] { (void)recomp::validate_function_map(map, code_start, code_end); });
        CHECK(e.code() == ErrorCode::map_invalid);
        CHECK_THAT(e.what(), StartsWith("unit.csv:2:") && ContainsSubstring("outside"));
        auto tail = map_of("a,0x00200FF0,0x00201010,32\n");
        CHECK(error_of([&] { (void)recomp::validate_function_map(tail, code_start, code_end); }).code() ==
              ErrorCode::map_invalid);
    }
    SECTION("a duplicate start") {
        auto map = map_of("a,0x00200000,0x00200010,16\nb,0x00200000,0x00200008,8\n");
        CHECK_THAT(error_of([&] { (void)recomp::validate_function_map(map, code_start, code_end); }).what(),
                   ContainsSubstring("like a"));
    }
    SECTION("overlapping functions") {
        auto map = map_of("a,0x00200000,0x00200020,32\nb,0x00200010,0x00200030,32\n");
        CHECK_THAT(error_of([&] { (void)recomp::validate_function_map(map, code_start, code_end); }).what(),
                   StartsWith("unit.csv:3:") && ContainsSubstring("overlaps a"));
    }
}

TEST_CASE("jump-table fragments parse with integer or string values", "[core][recomp][map]") {
    const auto tables = recomp::parse_jump_tables(R"(# synthetic
[[jump_tables.table]]
address = 0x00200800
base_register = 2
entries = [{index = 0, target = 0x00200040}, {index = 1, target = "0x00200050"}]

[[jump_tables.table]]
address = "0x00200900"
base_register = "3"
entries = [{target = 0x00200060}, {target = 0x00200070}]
)",
                                                  "unit.jump_tables.toml");
    REQUIRE(tables.size() == 2);
    CHECK(tables[0].address == 0x00200800);
    CHECK(tables[0].base_register == 2);
    REQUIRE(tables[0].entries.size() == 2);
    CHECK(tables[0].entries[1].target == 0x00200050);
    CHECK(tables[1].address == 0x00200900);
    CHECK(tables[1].base_register == 3);
    CHECK(tables[1].entries[1].index == 1); // position stands in for a missing index
    CHECK(tables[1].line == 7);

    CHECK(recomp::parse_jump_tables("", "empty.toml").empty());
    const Error bad =
        error_of([] { (void)recomp::parse_jump_tables("[[jump_tables.table]]\naddress = 1\n", "x.toml"); });
    CHECK(bad.code() == ErrorCode::map_parse);
    CHECK_THAT(bad.what(), StartsWith("x.toml:1:"));
    const Error syntax =
        error_of([] { (void)recomp::parse_jump_tables("[[jump_tables.table]\n", "y.toml"); });
    CHECK(syntax.code() == ErrorCode::map_parse);
    CHECK_THAT(syntax.what(), StartsWith("y.toml:"));
}

TEST_CASE("validate_function_map checks jump tables and link-return entries", "[core][recomp][map]") {
    auto map = map_of("a,0x00200000,0x00200100,256\n");
    recomp::JumpTable t;
    t.address = 0x00200800;
    t.base_register = 2;
    t.entries = {{0, 0x00200040}, {1, 0x00200050}};
    map.jump_tables = {t};
    map.link_return_entries = {0x00200048, 0x00200010, 0x00200048, 0x00200200};
    const auto warnings = recomp::validate_function_map(map, code_start, code_end);
    CHECK(map.link_return_entries == std::vector<std::uint32_t>{0x00200010, 0x00200048, 0x00200200});
    REQUIRE(warnings.size() == 1); // 0x00200200 lies in no mapped function
    CHECK_THAT(warnings[0], ContainsSubstring("0x00200200"));

    auto invalid = [&](auto mutate, const std::string& fragment) {
        auto copy = map;
        mutate(copy);
        const Error e = error_of([&] { (void)recomp::validate_function_map(copy, code_start, code_end); });
        CHECK(e.code() == ErrorCode::map_invalid);
        CHECK_THAT(e.what(), ContainsSubstring(fragment));
    };
    invalid([](recomp::FunctionMap& m) { m.jump_tables[0].address = 0; }, "misaligned address");
    invalid([](recomp::FunctionMap& m) { m.jump_tables[0].base_register = 32; }, "base_register");
    invalid([](recomp::FunctionMap& m) { m.jump_tables[0].entries.clear(); }, "no entries");
    invalid([](recomp::FunctionMap& m) { m.jump_tables[0].entries[1].index = 0; }, "repeats index");
    invalid([](recomp::FunctionMap& m) { m.jump_tables[0].entries[0].target = 0x00300000; }, "outside");
    invalid([](recomp::FunctionMap& m) { m.jump_tables.push_back(m.jump_tables[0]); }, "twice");
    invalid([](recomp::FunctionMap& m) { m.link_return_entries.push_back(0x00200002); }, "link-return");
}

TEST_CASE("map stats carry link-return entries and counts", "[core][recomp][map]") {
    recomp::FunctionMap map;
    recomp::parse_map_stats(
        "functions = 12\nunresolved_sites = 3\nlink_return_entries = [0x00200048, \"0x10\"]\n"
        "note = \"ignored\"\nholes = [{at = 0x00200010}]\n[counts]\nrows = 5\n",
        "unit.stats.toml", map);
    CHECK(map.link_return_entries == std::vector<std::uint32_t>{0x00200048, 0x10});
    REQUIRE(map.stats.size() == 3);
    CHECK(map.stats[0] == std::pair<std::string, std::int64_t>{"functions", 12});
    CHECK(map.stats[1].first == "unresolved_sites");
    CHECK(map.stats[2] == std::pair<std::string, std::int64_t>{"rows", 5}); // from [counts]
}

TEST_CASE("load_function_map reads the three files of a unit", "[core][recomp][map]") {
    const fs::path dir = fs::temp_directory_path() / "dq8-test-function-map";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream(dir / "title.csv") << csv("a,0x00200000,0x00200010,16\n");
        std::ofstream(dir / "title.jump_tables.toml")
            << "[[jump_tables.table]]\naddress = 0x00200800\nbase_register = 4\nentries = [{index = 0, "
               "target = "
               "0x00200000}]\n";
        std::ofstream(dir / "title.stats.toml") << "link_return_entries = [0x00200008]\n";
        std::ofstream(dir / "menu.csv") << csv("m,0x00200000,0x00200010,16\n");
    }
    const auto title = recomp::load_function_map(dir, "title");
    CHECK(title.unit == "title");
    CHECK(title.functions.size() == 1);
    CHECK(title.jump_tables.size() == 1);
    CHECK(title.link_return_entries.size() == 1);
    CHECK(title.stats_path == dir / "title.stats.toml");

    const auto menu = recomp::load_function_map(dir, "menu"); // the TOML files are optional
    CHECK(menu.jump_tables.empty());
    CHECK(menu.jump_tables_path.empty());

    const Error missing = error_of([&] { (void)recomp::load_function_map(dir, "shop"); });
    CHECK(missing.code() == ErrorCode::io_open);
    CHECK_THAT(missing.what(), ContainsSubstring("shop.csv"));
    fs::remove_all(dir);
}
