// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/elf_writer.h"
#include "dq8/core/error.h"
#include "dq8/core/hash.h"
#include "dq8/core/recomp.h"
#include "dq8/core/region_profile.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <toml.hpp>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using dq8::core::Error;
using dq8::core::ErrorCode;
namespace recomp = dq8::core::recomp;
namespace fs = std::filesystem;

// Synthetic published maps: made-up addresses in [0x00200000, 0x00201000), names from the runtime's call
// list.
namespace {

constexpr const char* sample_sha1 = "0123456789abcdef0123456789abcdef01234567";

std::string sample_text(const std::string& extra = "", const std::string& sha1 = sample_sha1) {
    return "format = 1\n"
           "profile = \"TEST_00000\"\n"
           "unit = \"main\"\n"
           "source_sha1 = \"" +
           sha1 +
           "\"\n"
           "code = [0x00200000, 0x00201000]\n"
           "functions = [\n"
           "    [0x00200000, 0x40],\n"
           "    [0x00200040, 0x40],\n"
           "    [0x00200100, 0x10],\n"
           "]\n"
           "link_return_entries = [0x00200048]\n"
           "\n"
           "[[named]]\n"
           "start = 0x00200100\n"
           "name = \"_FlushCache\"\n"
           "\n"
           "[[jump_table]]\n"
           "address = 0x00200800\n"
           "base_register = 2\n"
           "count = 3\n" +
           extra;
}

// A map with one replacement applied to the sample's text.
std::string with(const std::string& from, const std::string& to) {
    std::string text = sample_text();
    const std::size_t at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

ErrorCode code_of(const std::string& text) {
    try {
        recomp::parse_published_map(text, "test.toml");
    } catch (const Error& e) {
        return e.code();
    }
    FAIL("the map was accepted");
    return ErrorCode::io_open;
}

void put_words(std::vector<std::uint8_t>& bytes, std::size_t offset) {
    for (const std::uint32_t word : {0x00200040u, 0x00200050u, 0x00200060u}) {
        for (int i = 0; i < 4; ++i) {
            bytes[offset++] = static_cast<std::uint8_t>(word >> (8 * i));
        }
    }
}

// A unit ELF: 0x110 bytes of "code" at 0x00200000 and a data section at 0x00200800, each holding the three
// words 0x00200040, 0x00200050, 0x00200060 (at 0x00200100 and 0x00200800).
std::vector<std::uint8_t> sample_elf() {
    dq8::core::ElfWriter writer;
    writer.set_entry(0x00200000);
    dq8::core::ElfSection text;
    text.name = ".text";
    text.flags = dq8::core::elf_shf_alloc | dq8::core::elf_shf_execinstr;
    text.address = 0x00200000;
    text.data.assign(0x110, 0);
    put_words(text.data, 0x100);
    writer.add_section(std::move(text));
    dq8::core::ElfSection data;
    data.name = ".rodata";
    data.address = 0x00200800;
    data.data.assign(12, 0);
    put_words(data.data, 0);
    writer.add_section(std::move(data));
    return writer.build();
}

fs::path fresh_dir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("a published map parses into functions, names, link returns and table shapes",
          "[core][recomp][published]") {
    const recomp::PublishedMap map = recomp::parse_published_map(sample_text(), "test.toml");
    CHECK(map.profile == "TEST_00000");
    CHECK(map.unit == "main");
    CHECK(map.source_sha1 == sample_sha1);
    CHECK(map.code_start == 0x00200000);
    CHECK(map.code_end == 0x00201000);
    REQUIRE(map.functions.size() == 3);
    CHECK(map.functions[2].start == 0x00200100);
    CHECK(map.functions[2].size == 0x10);
    CHECK(map.link_return_entries == std::vector<std::uint32_t>{0x00200048});
    REQUIRE(map.named.size() == 1);
    CHECK(map.named[0].name == "_FlushCache");
    REQUIRE(map.jump_tables.size() == 1);
    CHECK(map.jump_tables[0].address == 0x00200800);
    CHECK(map.jump_tables[0].base_register == 2);
    CHECK(map.jump_tables[0].count == 3);
}

TEST_CASE("a published map holds only the format's keys and values", "[core][recomp][published]") {
    // Syntax, unknown keys and the wrong types.
    CHECK(code_of(sample_text("comment = \"text\"\n")) == ErrorCode::map_parse);
    CHECK(code_of(with("count = 3\n", "count = 3\nentries = [0x00200040]\n")) == ErrorCode::map_parse);
    CHECK(code_of(with("format = 1", "format = 2")) == ErrorCode::map_parse);
    CHECK(code_of(with("[0x00200000, 0x40]", "[\"0x00200000\", 0x40]")) == ErrorCode::map_parse);
    CHECK(code_of(with("[0x00200000, 0x40]", "[0x00200000, 0x40, 1]")) == ErrorCode::map_parse);
    CHECK(code_of(with("0123456789abcdef", "0123456789ABCDEF")) == ErrorCode::map_parse);
    CHECK(code_of(with("unit = \"main\"", "unit = \"Main\"")) == ErrorCode::map_parse);
    CHECK(code_of(with("name = \"_FlushCache\"\n", "name = \"_FlushCache\"\nnote = \"x\"\n")) ==
          ErrorCode::map_parse);
    CHECK(code_of(with("base_register = 2\n", "")) == ErrorCode::map_parse);
    // Inconsistencies.
    CHECK(code_of(with("[0x00200040, 0x40]", "[0x00200020, 0x40]")) == ErrorCode::map_invalid); // overlap
    CHECK(code_of(with("[0x00200100, 0x10]", "[0x00200010, 0x10]")) == ErrorCode::map_invalid); // order
    CHECK(code_of(with("[0x00200100, 0x10]", "[0x00200FF8, 0x10]")) == ErrorCode::map_invalid); // outside
    CHECK(code_of(with("[0x00200100, 0x10]", "[0x00200100, 0x0]")) == ErrorCode::map_invalid);
    CHECK(code_of(with("[0x00200100, 0x10]", "[0x00200102, 0x10]")) == ErrorCode::map_invalid);
    CHECK(code_of(with("[0x00200048]", "[0x00201000]")) == ErrorCode::map_invalid);
    CHECK(code_of(with("start = 0x00200100", "start = 0x00200104")) == ErrorCode::map_invalid);
    CHECK(code_of(with("_FlushCache", "UpdateScore")) == ErrorCode::map_invalid); // not a call-list name
    CHECK(code_of(with("_FlushCache", "FUN_00200100")) == ErrorCode::map_invalid);
    CHECK(code_of(with("base_register = 2", "base_register = 32")) == ErrorCode::map_invalid);
    CHECK(code_of(with("count = 3", "count = 0")) == ErrorCode::map_invalid);
    CHECK(code_of(with("address = 0x00200800", "address = 0x00200802")) == ErrorCode::map_invalid);
    CHECK(code_of(sample_text("\n[[jump_table]]\naddress = 0x00200800\nbase_register = 2\ncount = 1\n")) ==
          ErrorCode::map_invalid); // the same table twice
    CHECK(code_of(with("code = [0x00200000, 0x00201000]", "code = [0x00201000, 0x00200000]")) ==
          ErrorCode::map_invalid);

    try {
        recomp::parse_published_map(with("count = 3", "count = 0"), "maps/x.toml");
        FAIL("accepted");
    } catch (const Error& e) {
        CHECK_THAT(e.what(), ContainsSubstring("maps/x.toml:"));
    }
}

TEST_CASE("prepare's function map takes names from the map and table entries from the unit image",
          "[core][recomp][published]") {
    const recomp::PublishedMap published = recomp::parse_published_map(sample_text(), "test.toml");
    const std::vector<std::uint8_t> elf = sample_elf();
    recomp::FunctionMap map = recomp::make_function_map(published, elf);
    REQUIRE(map.functions.size() == 3);
    CHECK(map.functions[0].name == "FUN_00200000");
    CHECK(map.functions[1].name == "FUN_00200040");
    CHECK(map.functions[1].end == 0x00200080);
    CHECK(map.functions[2].name == "_FlushCache");
    CHECK(map.link_return_entries == std::vector<std::uint32_t>{0x00200048});
    REQUIRE(map.jump_tables.size() == 1);
    const recomp::JumpTable& t = map.jump_tables[0];
    CHECK(t.address == 0x00200800);
    CHECK(t.base_register == 2);
    REQUIRE(t.entries.size() == 3);
    CHECK(t.entries[0].index == 0);
    CHECK(t.entries[0].target == 0x00200040);
    CHECK(t.entries[2].index == 2);
    CHECK(t.entries[2].target == 0x00200060);
    CHECK(recomp::validate_function_map(map, published.code_start, published.code_end).empty());

    // The CSV the recompiler reads, in the contributor maps' spelling.
    CHECK(recomp::function_csv_text(map) == "Name,Start,End,Size\n"
                                            "FUN_00200000,0x00200000,0x00200040,64\n"
                                            "FUN_00200040,0x00200040,0x00200080,64\n"
                                            "_FlushCache,0x00200100,0x00200110,16\n");
    CHECK(recomp::parse_function_csv(recomp::function_csv_text(map), "x.csv").size() == 3);
    CHECK(recomp::generated_function_name(0x0010ABC0) == "FUN_0010abc0");

    // A table whose words the image does not hold.
    const recomp::PublishedMap beyond = recomp::parse_published_map(with("count = 3", "count = 4"), "t.toml");
    try {
        recomp::make_function_map(beyond, elf);
        FAIL("accepted");
    } catch (const Error& e) {
        CHECK(e.code() == ErrorCode::map_invalid);
        CHECK_THAT(e.what(), ContainsSubstring("entry 3 at 0x0020080C"));
    }
}

TEST_CASE("published maps are found by profile and unit and matched to the unit file",
          "[core][recomp][published]") {
    const fs::path dir = fresh_dir("dq8-test-published-maps");
    CHECK(recomp::published_map_file(dir, "TEST_00000", "main") == dir / "TEST_00000" / "main.toml");
    try {
        recomp::load_published_map(dir, "TEST_00000", "main");
        FAIL("accepted");
    } catch (const Error& e) {
        CHECK(e.code() == ErrorCode::maps_missing);
    }
    fs::create_directories(dir / "TEST_00000");
    std::ofstream(dir / "TEST_00000" / "main.toml") << sample_text();
    const recomp::PublishedMap map = recomp::load_published_map(dir, "TEST_00000", "main");
    CHECK(map.path == dir / "TEST_00000" / "main.toml");
    recomp::check_published_map_source(map, "0123456789ABCDEF0123456789ABCDEF01234567"); // any case
    try {
        recomp::check_published_map_source(map, "ffffffffffffffffffffffffffffffffffffffff");
        FAIL("accepted");
    } catch (const Error& e) {
        CHECK(e.code() == ErrorCode::maps_missing);
    }
    fs::copy_file(dir / "TEST_00000" / "main.toml", dir / "TEST_00000" / "title.toml");
    try {
        recomp::load_published_map(dir, "TEST_00000", "title"); // a file under the wrong name
        FAIL("accepted");
    } catch (const Error& e) {
        CHECK(e.code() == ErrorCode::map_invalid);
    }

    // An install: <prefix>/bin/<program> finds <prefix>/share/dq8/maps.
    fs::create_directories(dir / "prefix" / "bin");
    fs::create_directories(dir / "prefix" / "share" / "dq8" / "maps");
    const fs::path cwd = fs::current_path();
    fs::current_path(dir); // no data/maps in the working directory
    CHECK(recomp::find_maps_dir(dir / "prefix" / "bin") == dir / "prefix" / "share" / "dq8" / "maps");
    CHECK_THROWS_AS(recomp::find_maps_dir(dir / "elsewhere"), Error);
    fs::current_path(cwd);
    fs::remove_all(dir);
}

TEST_CASE("prepare reads a published map, writes the recompiler's CSV and fills the tables",
          "[core][recomp][published][pipeline]") {
    const fs::path dir = fresh_dir("dq8-test-published-prepare");
    const std::vector<std::uint8_t> elf = sample_elf();
    fs::create_directories(dir / "in");
    std::ofstream(dir / "in" / "main.elf", std::ios::binary)
        .write(reinterpret_cast<const char*>(elf.data()), static_cast<std::streamsize>(elf.size()));
    const std::string sha1 = dq8::core::to_hex(dq8::core::Sha1::of(elf));
    const recomp::UnitSource source = recomp::elf_units({{"main", dir / "in" / "main.elf"}}, "TEST_00000");
    REQUIRE(source.units.size() == 1);
    fs::create_directories(dir / "maps" / "TEST_00000");
    // The developer unit's code range is its executable segment, [0x00200000, 0x00200110).
    std::string text = sample_text("", sha1);
    text.replace(text.find("0x00201000]"), 11, "0x00200110]");
    text.replace(text.find("address = 0x00200800"), 20, "address = 0x00200100"); // words inside .text
    std::ofstream(dir / "maps" / "TEST_00000" / "main.toml") << text;

    recomp::PipelineOptions o;
    o.work_dir = dir / "work";
    o.published_maps_dir = dir / "maps";
    const recomp::PrepareReport report = recomp::prepare_units(source, o);
    REQUIRE(report.units.size() == 1);
    const recomp::PreparedUnit& u = report.units[0];
    CHECK(u.functions == 3);
    CHECK(u.jump_tables == 1);
    CHECK(u.stubs == 1);
    CHECK(u.map_csv == fs::absolute(dir / "work" / "units" / "main.csv"));
    CHECK(u.published_map == fs::absolute(dir / "maps" / "TEST_00000" / "main.toml"));
    std::ifstream csv(u.map_csv);
    std::stringstream csv_text;
    csv_text << csv.rdbuf();
    CHECK_THAT(csv_text.str(), ContainsSubstring("FUN_00200040,0x00200040,0x00200080,64\n"));
    std::ifstream config(u.config);
    std::stringstream config_text;
    config_text << config.rdbuf();
    std::istringstream in(config_text.str());
    const toml::value parsed = toml::parse(in, "config.toml");
    const auto& tables = parsed.at("jump_tables").at("table").as_array();
    REQUIRE(tables.size() == 1);
    REQUIRE(tables[0].at("entries").as_array().size() == 3); // the words in the image's .text
    CHECK(tables[0].at("entries").as_array()[1].at("target").as_integer() == 0x00200050);

    // A map published for another file is refused with maps-missing; so is a missing map.
    std::ofstream(dir / "maps" / "TEST_00000" / "main.toml") << sample_text("", std::string(40, 'a'));
    try {
        recomp::prepare_units(source, o);
        FAIL("accepted");
    } catch (const Error& e) {
        CHECK(e.code() == ErrorCode::maps_missing);
    }
    fs::remove(dir / "maps" / "TEST_00000" / "main.toml");
    try {
        recomp::prepare_units(source, o);
        FAIL("accepted");
    } catch (const Error& e) {
        CHECK(e.code() == ErrorCode::maps_missing);
    }
    o.published_maps_dir.clear();
    CHECK_THROWS_AS(recomp::prepare_units(source, o), Error); // no maps at all
    fs::remove_all(dir);
}

TEST_CASE("the published maps in data/maps follow the format and match a region profile",
          "[core][recomp][published]") {
    const fs::path root = fs::path(DQ8_SOURCE_DIR) / "data" / "maps";
    const std::vector<dq8::core::RegionProfile> profiles =
        dq8::core::load_region_profiles(fs::path(DQ8_SOURCE_DIR) / "data" / "regions");
    std::size_t files = 0;
    for (const fs::directory_entry& profile_dir : fs::directory_iterator(root)) {
        if (!profile_dir.is_directory()) {
            continue;
        }
        const std::string profile_id = profile_dir.path().filename().string();
        const auto profile =
            std::find_if(profiles.begin(), profiles.end(),
                         [&](const dq8::core::RegionProfile& p) { return p.id == profile_id; });
        REQUIRE(profile != profiles.end());
        std::set<std::string> sha1s{profile->boot.sha1};
        for (const dq8::core::ProfileOverlay& o : profile->overlays) {
            sha1s.insert(o.sha1);
        }
        for (const fs::directory_entry& file : fs::directory_iterator(profile_dir.path())) {
            const std::string unit = file.path().stem().string();
            INFO(file.path().string());
            const recomp::PublishedMap map = recomp::load_published_map(root, profile_id, unit);
            CHECK_FALSE(map.functions.empty());
            CHECK(sha1s.count(map.source_sha1) == 1);
            ++files;
        }
    }
    CHECK(files >= 7); // SLUS_21207: main and six overlays
}
