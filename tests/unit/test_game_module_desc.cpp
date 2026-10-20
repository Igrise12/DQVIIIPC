// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Module descriptors: validate_descriptor() against a unit spec, the game layer's specs from a region
// profile, and loading real shared libraries (the contract unit, built by this test tree as a game module).
#include "dq8/abi/ps2recomp/r5900_context.h"
#include "dq8/core/region_profile.h"
#include "game/unit_specs.h"
#include "runtime/ee/game_module.h"
#include "runtime/platform/dynlib.h"
#include "support/contract_unit.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace dq8::rt;

std::array<dq8_guest_fn, 8> g_slots{};

void entry_fn(std::uint8_t*, R5900Context*, PS2Runtime*) {}

constexpr char sha1[] = "89abcdef0123456789abcdef0123456789abcdef";

// A valid main-unit descriptor over a synthetic table [0x00100000, 0x00100020) with the entry at +8.
dq8_game_module_desc good_main() {
    g_slots.fill(nullptr);
    g_slots[2] = entry_fn;
    dq8_game_module_desc d{};
    d.magic = DQ8_GAME_MODULE_MAGIC;
    d.abi_version = DQ8_GAME_MODULE_ABI_VERSION;
    d.struct_size = sizeof(dq8_game_module_desc);
    d.unit_kind = DQ8_UNIT_MAIN;
    d.overlay_id = 0;
    d.unit_name = "main";
    d.profile_id = "TEST_00001";
    d.source_sha1 = sha1;
    d.generator = "test";
    d.context_size = sizeof(R5900Context);
    d.context_align = alignof(R5900Context);
    d.entry_point = 0x00100008u;
    d.table_base = 0x00100000u;
    d.table_end = 0x00100020u;
    d.table_slot_count = 8;
    d.table = g_slots.data();
    return d;
}

UnitSpec main_spec() {
    UnitSpec s;
    s.name = "main";
    s.kind = DQ8_UNIT_MAIN;
    s.profile_id = "TEST_00001";
    s.source_sha1 = "89ABCDEF0123456789ABCDEF0123456789ABCDEF"; // compared case-insensitively
    s.entry_point = 0x00100008u;
    s.range_begin = 0x00100000u;
    s.range_end = 0x00200000u;
    return s;
}

bool has(const std::vector<std::string>& problems, std::string_view needle) {
    return std::any_of(problems.begin(), problems.end(),
                       [&](const std::string& p) { return p.find(needle) != std::string::npos; });
}

} // namespace

TEST_CASE("descriptor: a matching descriptor has no problems", "[module]") {
    const dq8_game_module_desc d = good_main();
    CHECK(validate_descriptor(&d, main_spec()).empty());
}

TEST_CASE("descriptor: magic, ABI and size problems are reported alone", "[module]") {
    CHECK(validate_descriptor(nullptr, main_spec()) ==
          std::vector<std::string>{"dq8_game_module_describe returned null"});
    dq8_game_module_desc d = good_main();
    d.magic = 0x12345678u;
    d.unit_name = nullptr; // not reported: nothing after the magic can be trusted
    auto p = validate_descriptor(&d, main_spec());
    REQUIRE(p.size() == 1);
    CHECK(p[0].find("not a DQ8 game module") != std::string::npos);

    d = good_main();
    d.abi_version = DQ8_GAME_MODULE_ABI_VERSION + 1;
    p = validate_descriptor(&d, main_spec());
    REQUIRE(p.size() == 1);
    CHECK(p[0].find("rebuild the module") != std::string::npos);

    d = good_main();
    d.struct_size = 12;
    p = validate_descriptor(&d, main_spec());
    REQUIRE(p.size() == 1);
    CHECK(p[0].find("descriptor size is 12 bytes") != std::string::npos);
}

TEST_CASE("descriptor: every field problem is reported in one pass", "[module]") {
    dq8_game_module_desc d = good_main();
    d.unit_kind = DQ8_UNIT_OVERLAY;
    d.overlay_id = 3;
    d.unit_name = "battle";
    d.profile_id = nullptr;
    d.source_sha1 = "not-a-hash";
    d.generator = "";
    d.context_size = 16;
    d.entry_point = 0x00100004u;
    d.table_end = 0x00300002u;
    d.table = nullptr;
    const auto p = validate_descriptor(&d, main_spec());
    CHECK(has(p, "unit kind is overlay, expected main"));
    CHECK(has(p, "overlay id is 3, expected 0"));
    CHECK(has(p, "unit name is \"battle\", expected \"main\""));
    CHECK(has(p, "profile id is null"));
    CHECK(has(p, "is not 40 hex digits"));
    CHECK(has(p, "generator is empty"));
    CHECK(has(p, "R5900Context is 16 bytes"));
    CHECK(has(p, "entry point is 0x00100004, expected 0x00100008"));
    CHECK(has(p, "is not 4-aligned"));
    CHECK(has(p, "table slot count is 8"));
    CHECK(has(p, "table pointer is null"));
    CHECK(has(p, "outside the unit's range"));
    CHECK(p.size() == 12);
}

TEST_CASE("descriptor: identity, range and entry checks", "[module]") {
    dq8_game_module_desc d = good_main();
    SECTION("a module built from another file") {
        d.source_sha1 = "0000000000000000000000000000000000000000";
        CHECK(has(validate_descriptor(&d, main_spec()), "built from a different file"));
    }
    SECTION("unchecked fields in developer mode") {
        UnitSpec dev;
        dev.name = "main";
        dev.entry_point = 0x00100008u;
        d.profile_id = "ANYTHING";
        CHECK(validate_descriptor(&d, dev).empty());
        d.profile_id = "";
        CHECK(validate_descriptor(&d, dev).empty());
        CHECK(has(validate_descriptor(&d, main_spec()), "profile id is empty"));
    }
    SECTION("the entry point must have a function") {
        g_slots[2] = nullptr;
        CHECK(
            has(validate_descriptor(&d, main_spec()), "entry point 0x00100008 has no function in the table"));
    }
    SECTION("the entry point must lie in the table") {
        d.entry_point = 0x00100040u;
        UnitSpec s = main_spec();
        s.entry_point = 0x00100040u;
        CHECK(has(validate_descriptor(&d, s), "is outside the table"));
    }
    SECTION("an empty table") {
        d.table_end = d.table_base;
        d.table_slot_count = 0;
        CHECK(has(validate_descriptor(&d, main_spec()), "is empty"));
    }
}

TEST_CASE("game layer: unit specs from a region profile", "[module][game]") {
    dq8::core::RegionProfile p;
    p.id = "TEST_00001";
    p.boot.sha1 = "da39a3ee5e6b4b0d3255bfef95601890afd80709";
    p.boot.entry = 0x00100008u;
    p.boot.load_address = 0x00100000u;
    p.boot.main_memsz = 0x1000u;
    p.overlay_window.base = 0x00200000u;
    p.overlay_window.end = 0x00210000u;
    dq8::core::ProfileOverlay b;
    b.name = "BRAVO";
    b.id = 2;
    b.index = 1;
    b.segment_memsz = 0x300;
    b.sha1 = "1111111111111111111111111111111111111111";
    dq8::core::ProfileOverlay a = b;
    a.name = "ALPHA";
    a.id = 7;
    a.index = 0;
    a.segment_memsz = 0;
    a.sha1.clear();
    p.overlays = {b, a}; // file order differs from index order

    {
        const OverlayWindowSpec w = dq8::game::overlay_window_spec(p);
        CHECK(w.base == 0x00200000u);
        CHECK(w.end == 0x00210000u);
    }
    const std::vector<UnitSpec> specs = dq8::game::unit_specs(p);
    REQUIRE(specs.size() == 3);
    CHECK(specs[0].name == "main");
    CHECK(specs[0].kind == DQ8_UNIT_MAIN);
    CHECK(specs[0].entry_point == 0x00100008u);
    CHECK(specs[0].range_begin == 0x00100000u);
    CHECK(specs[0].range_end == 0x00101000u);
    CHECK(specs[0].source_sha1 == p.boot.sha1);
    CHECK(specs[0].profile_id == "TEST_00001");
    CHECK(specs[1].name == "alpha");
    CHECK(specs[1].kind == DQ8_UNIT_OVERLAY);
    CHECK(specs[1].overlay_id == 7);
    CHECK(specs[1].entry_point == 0u);
    CHECK(specs[1].range_end == 0x00210000u); // no recorded memsz: the whole window
    CHECK(specs[1].source_sha1.empty());
    CHECK(specs[2].name == "bravo");
    CHECK(specs[2].range_begin == 0x00200000u);
    CHECK(specs[2].range_end == 0x00200300u); // its own segment
}

TEST_CASE("game layer: the shipped region profile gives seven units", "[module][game]") {
    const auto profile = dq8::core::load_region_profile(std::filesystem::path(DQ8_SOURCE_DIR) / "data" /
                                                        "regions" / "SLUS_21207.toml");
    const std::vector<UnitSpec> specs = dq8::game::unit_specs(profile);
    std::vector<std::string> names;
    for (const UnitSpec& s : specs) {
        names.push_back(s.name);
        CHECK(s.profile_id == profile.id);
        CHECK(s.source_sha1.size() == 40);
    }
    CHECK(names == std::vector<std::string>{"main", "title", "casino", "viewer", "battle", "menu", "shop"});
    CHECK(specs[0].entry_point == profile.boot.entry);
    for (std::size_t i = 1; i < specs.size(); ++i) {
        CHECK(specs[i].range_begin == profile.overlay_window.base);
        CHECK(specs[i].range_end <= profile.overlay_window.end);
        CHECK(specs[0].range_end <= specs[i].range_begin); // the main unit's range ends before the window
    }
}

TEST_CASE("module files: names and discovery", "[module]") {
    const std::filesystem::path file = module_path("mods", "battle");
    CHECK(file.parent_path() == std::filesystem::path("mods"));
    CHECK(unit_name_from_module_file(file) == "battle");
    CHECK(unit_name_from_module_file("mods/libsomething.so").empty());
    CHECK(unit_name_from_module_file(module_path("x", "")).empty());
}

TEST_CASE("module loading: the contract unit as a real shared library", "[module]") {
    const std::filesystem::path dir = DQ8_CONTRACT_MODULE_DIR;
    REQUIRE(std::filesystem::is_regular_file(module_path(dir, contract::unit_name)));
    const std::vector<std::filesystem::path> found = find_module_files(dir);
    REQUIRE(found.size() == 1);
    CHECK(unit_name_from_module_file(found[0]) == contract::unit_name);

    UnitSpec spec;
    spec.name = contract::unit_name;
    spec.source_sha1 = contract::source_sha1;
    spec.entry_point = contract::entry;
    spec.range_begin = contract::table_base;
    spec.range_end = contract::table_end;
    ModuleSet ok = load_modules(std::vector<UnitSpec>{spec}, dir);
    CHECK(ok.problems.empty());
    REQUIRE(ok.units.size() == 1);
    const dq8_game_module_desc& d = ok.units[0].module.descriptor();
    CHECK(descriptor_string(d.unit_name) == contract::unit_name);
    CHECK(d.table_slot_count == (contract::table_end - contract::table_base) / 4u);
    CHECK(d.table[0] != nullptr); // filled by the module's static initialiser

    UnitSpec wrong = spec;
    wrong.source_sha1 = "fedcba9876543210fedcba9876543210fedcba98";
    UnitSpec missing = spec;
    missing.name = "nosuchunit";
    const ModuleSet bad = load_modules(std::vector<UnitSpec>{wrong, missing}, dir);
    CHECK(bad.units.empty());
    REQUIRE(bad.problems.size() == 2);
    CHECK(bad.problems[0].find("contract: source SHA-1") != std::string::npos);
    CHECK(bad.problems[1].find("nosuchunit: module not found") != std::string::npos);
}

TEST_CASE("module loading: a library that does not load", "[module]") {
    CHECK_THROWS_AS(GameModule::open("does/not/exist/dq8_unit_x.so"), DynlibError);
}
