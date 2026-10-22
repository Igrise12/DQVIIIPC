// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/elf_writer.h"
#include "dq8/core/error.h"
#include "dq8/core/recomp.h"
#include "support/iso_builder.h"
#include "support/mips_asm.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <toml.hpp>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using dq8::core::Error;
using dq8::core::ErrorCode;
namespace recomp = dq8::core::recomp;
namespace fs = std::filesystem;

// Synthetic maps only: made-up addresses, our own names plus a few names from the emitter's public call list.
namespace {

recomp::FunctionMap sample_map() {
    recomp::FunctionMap map;
    map.unit = "sample";
    map.functions = {
        {"FUN_00200000", 0x00200000, 0x00200040, 2},
        {"_FlushCache", 0x00200100, 0x00200110, 5}, // kernel call, leading-underscore alias
        {"malloc_r", 0x00200080, 0x00200100, 4},    // library stub
        {"helper", 0x00200040, 0x00200080, 3},
        {"memalign_r", 0x00200110, 0x00200120, 6},
    };
    recomp::JumpTable t;
    t.address = 0x00200800;
    t.base_register = 2;
    t.entries = {{0, 0x00200040}, {1, 0x00200050}};
    map.jump_tables = {t};
    map.link_return_entries = {0x00200048, 0x00200088};
    return map;
}

toml::value parse(const std::string& text) {
    std::istringstream in(text);
    return toml::parse(in, "config.toml");
}

fs::path fresh_dir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::string generic(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

} // namespace

TEST_CASE("runtime call names resolve like the emitter resolves them", "[core][recomp][config]") {
    using recomp::RuntimeCallKind;
    CHECK(recomp::resolve_runtime_call("FlushCache").kind == RuntimeCallKind::syscall);
    CHECK(recomp::resolve_runtime_call("_FlushCache").name == "FlushCache");
    CHECK(recomp::resolve_runtime_call("malloc_r").kind == RuntimeCallKind::stub);
    CHECK(recomp::resolve_runtime_call("_malloc_r").name == "malloc_r");
    CHECK(recomp::resolve_runtime_call("___malloc_lock").name == "__malloc_lock"); // one underscore dropped
    CHECK(recomp::resolve_runtime_call("_malloc_lock").kind == RuntimeCallKind::none); // never two added
    CHECK(recomp::resolve_runtime_call("FUN_00100000").kind == RuntimeCallKind::none);
    CHECK(recomp::resolve_runtime_call("").kind == RuntimeCallKind::none);
    CHECK(recomp::resolve_runtime_call("__FlushCache").kind == RuntimeCallKind::none); // only one underscore
}

TEST_CASE("stub bindings name every call-list function by start address", "[core][recomp][config]") {
    const auto stubs = recomp::stub_bindings(sample_map());
    CHECK(stubs ==
          std::vector<std::string>{"malloc_r@0x00200080", "FlushCache@0x00200100", "memalign_r@0x00200110"});
}

TEST_CASE("the recompiler config carries the fields ConfigManager reads", "[core][recomp][config]") {
    const recomp::FunctionMap map = sample_map();
    recomp::RecompConfigInput in;
    in.unit = "sample";
    in.profile_id = "TEST_00000";
    in.elf = fs::path("work") / "units" / "sample.elf";
    in.output_dir = fs::path("work") / "generated" / "sample";
    in.map_csv = fs::path("maps") / "sample.csv";
    in.map = &map;
    in.skip = {"broken@0x00200040"};
    const recomp::RecompConfig config = recomp::make_recomp_config(in);
    CHECK_THAT(config.toml, StartsWith("# Recompiler configuration for code unit 'sample'"));
    CHECK(config.stubs.size() == 3);
    CHECK(config.entry_points == std::vector<std::string>{"0x00200048", "0x00200088"});
    CHECK(config.jump_tables == 1);
    CHECK_THAT(config.toml, ContainsSubstring("0x00200800"));

    // Read it back the way PS2Recomp's ConfigManager does (same library, same calls).
    const toml::value data = parse(config.toml);
    const toml::value& general = data.at("general");
    CHECK(toml::find<std::string>(general, "input") == in.elf.string());
    CHECK(toml::find<std::string>(general, "output") == in.output_dir.string());
    CHECK(toml::find<std::string>(general, "ghidra_output") == in.map_csv.string());
    CHECK_FALSE(toml::find<bool>(general, "single_file_output"));
    CHECK_FALSE(toml::find<bool>(general, "patch_syscalls"));
    CHECK(toml::find<bool>(general, "patch_cop0"));
    CHECK(toml::find<bool>(general, "patch_cache"));
    CHECK(toml::find<std::vector<std::string>>(general, "stubs") == config.stubs);
    CHECK(toml::find<std::vector<std::string>>(general, "entry_points") == config.entry_points);
    CHECK(toml::find<std::vector<std::string>>(general, "skip") == in.skip);
    const auto& tables = data.at("jump_tables").at("table").as_array();
    REQUIRE(tables.size() == 1);
    CHECK(tables[0].at("address").as_integer() == 0x00200800);
    CHECK(tables[0].at("base_register").as_integer() == 2);
    const auto& entries = tables[0].at("entries").as_array();
    REQUIRE(entries.size() == 2);
    CHECK(entries[1].at("index").as_integer() == 1);
    CHECK(entries[1].at("target").as_integer() == 0x00200050);
}

TEST_CASE("a map without jump tables writes no [jump_tables]", "[core][recomp][config]") {
    recomp::FunctionMap map;
    map.functions = {{"a", 0x00200000, 0x00200010, 2}};
    recomp::RecompConfigInput in;
    in.map = &map;
    const recomp::RecompConfig config = recomp::make_recomp_config(in);
    CHECK_FALSE(parse(config.toml).contains("jump_tables"));
    CHECK(config.stubs.empty());

    recomp::RecompConfigInput none;
    CHECK_THROWS_AS(recomp::make_recomp_config(none), Error);
}

TEST_CASE("the recompiler report is parsed into named counts and events", "[core][recomp][driver]") {
    // The shape of PS2Recomp's printSummary(), with made-up numbers.
    const std::string output =
        "[recompiler] parsing config\n"
        "Functions discovered: 1\n" // outside the block: ignored
        "\n========== PS2Recomp report ==========\r\n"
        "Functions discovered: 120\n"
        "Symbols: 0, sections: 4, relocations: 0\n"
        "Functions processed: 120, recompiled: 110, stubs: 7, skipped: 3, decode failures: 0\n"
        "Additional entrypoints: 9\n"
        "Generated functions: 129\n"
        "Indirect fallback promotions: 2 (14 fallback entries)\n"
        "Unhandled instructions: 5\n"
        "Correctness-critical guest fallbacks: 1, failures: 0\n"
        "Warnings: 3, errors: 5\n"
        "\nEvents:\n"
        "  [info] elf - ELF entry point: 0x200000\n"
        "  [warning] ghidra-map - Ignored 2 Ghidra function(s) outside executable sections.\n"
        "  [warning] control-flow function=f addr=0x200010 - unresolved JR/JALR at 0x200010\n"
        "  [error] unhandled-instruction function=g addr=0x200020 - x raw=0x0\n"
        "======================================\n"
        "Recompilation completed successfully\n";
    const recomp::RecompReport r = recomp::parse_recomp_report(output);
    REQUIRE(r.found);
    CHECK(r.count("functions_discovered") == 120);
    CHECK(r.count("sections") == 4);
    CHECK(r.count("functions_processed") == 120);
    CHECK(r.count("functions_recompiled") == 110);
    CHECK(r.count("functions_stubbed") == 7);
    CHECK(r.count("functions_skipped") == 3);
    CHECK(r.count("decode_failures") == 0);
    CHECK(r.count("additional_entrypoints") == 9);
    CHECK(r.count("generated_functions") == 129);
    CHECK(r.count("indirect_fallback_promotions") == 2);
    CHECK(r.count("indirect_fallback_entries") == 14);
    CHECK(r.count("unhandled_instructions") == 5);
    CHECK(r.count("correctness_critical_guest_fallbacks") == 1);
    CHECK(r.count("correctness_critical_failures") == 0);
    CHECK(r.count("warnings") == 3);
    CHECK(r.count("errors") == 5);
    CHECK(r.count("no_such_key") == 0);
    CHECK(r.events_by_category.at("warning:ghidra-map") == 1);
    CHECK(r.events_by_category.at("error:unhandled-instruction") == 1);
    CHECK(r.events_by_category.at("info:elf") == 1); // counted, not listed
    REQUIRE(r.notable_events.size() == 2);           // unhandled instructions are counted, not listed
    CHECK(r.notable_events[0].category == "ghidra-map");
    CHECK_THAT(r.notable_events[1].text, ContainsSubstring("unresolved JR/JALR"));

    CHECK_FALSE(recomp::parse_recomp_report("Error: cannot open config\n").found);
}

TEST_CASE("run_recompiler swaps output in and keeps unchanged files' times", "[core][recomp][driver]") {
    // The test child plays ps2_recomp when its only argument is a .toml file (see process_child.cpp).
    const fs::path dir = fresh_dir("dq8-test-recomp-driver");
    const fs::path out = dir / "generated" / "unit";
    const fs::path config = dir / "unit.toml";
    auto write_config = [&](const std::string& variant, bool fail) {
        std::ofstream(config) << "output = \"" << generic(out) << "\"\nvariant = \"" << variant << "\"\n"
                              << (fail ? "fail = true\n" : "");
    };
    recomp::GenerateOptions o;
    o.ps2_recomp = DQ8_TEST_CHILD;
    o.config = config;
    o.output_dir = out;
    o.log_file = dir / "logs" / "unit.generate.log";

    write_config("one", false);
    const recomp::GenerateResult first = recomp::run_recompiler(o);
    CHECK(first.report.found);
    CHECK(first.report.count("functions_recompiled") == 2);
    CHECK(first.files == 3);
    CHECK(first.files_unchanged == 0);
    CHECK(fs::is_regular_file(o.log_file));

    // Age the files so that a rewrite would show, and leave a file the next run will not produce.
    const auto old_time = fs::last_write_time(out / "a.cpp") - std::chrono::hours(1);
    for (const char* name : {"a.cpp", "b.cpp", "register_functions.cpp"}) {
        fs::last_write_time(out / name, old_time);
    }
    std::ofstream(out / "stale.cpp") << "// gone next run\n";
    write_config("two", false);
    const recomp::GenerateResult second = recomp::run_recompiler(o);
    CHECK(second.files == 3);
    CHECK(second.files_unchanged == 2); // a.cpp and register_functions.cpp
    CHECK(second.files_removed == 1);
    // Unexpanded: libc++ on Windows counts file times in __int128, which Catch2 cannot print.
    CHECK((fs::last_write_time(out / "a.cpp") == old_time));
    CHECK((fs::last_write_time(out / "b.cpp") != old_time));
    CHECK_FALSE(fs::exists(out / "stale.cpp"));
    CHECK_FALSE(fs::exists(fs::path(out.string() + ".previous")));

    // A failed run leaves the previous output in place, untouched.
    write_config("three", true);
    try {
        (void)recomp::run_recompiler(o);
        FAIL("expected recomp_failed");
    } catch (const Error& e) {
        CHECK(e.code() == ErrorCode::recomp_failed);
        CHECK_THAT(e.what(), ContainsSubstring("status 1") && ContainsSubstring("Recompilation failed"));
    }
    std::ifstream b(out / "b.cpp");
    const std::string kept((std::istreambuf_iterator<char>(b)), std::istreambuf_iterator<char>());
    CHECK(kept == "// b two\n");
    CHECK((fs::last_write_time(out / "a.cpp") == old_time));
    b.close();

    o.ps2_recomp = dir / "no-such-recompiler";
    CHECK_THROWS_AS(recomp::run_recompiler(o), Error);
    CHECK(fs::is_regular_file(out / "b.cpp")); // restored after the failed start too
    fs::remove_all(dir);
}

TEST_CASE("default_build_jobs follows cores and free memory", "[core][recomp][build]") {
    constexpr std::uint64_t gib = std::uint64_t{1} << 30;
    CHECK(recomp::default_build_jobs(12, 16 * gib) == 12);     // (16 - 3) / 0.4 = 32 -> capped by cores
    CHECK(recomp::default_build_jobs(12, 6 * gib) == 7);       // (6 - 3) / 0.4 = 7.5
    CHECK(recomp::default_build_jobs(12, 3 * gib) == 1);       // nothing to spare
    CHECK(recomp::default_build_jobs(12, 2 * gib) == 1);       // never below one
    CHECK(recomp::default_build_jobs(12, std::nullopt) == 12); // memory unknown
    CHECK(recomp::default_build_jobs(0, std::nullopt) == 1);
    CHECK(recomp::default_build_jobs() >= 1);
}

TEST_CASE("a module build tree configured from a moved template is configured afresh",
          "[core][recomp][build]") {
    const dq8::test::TempDir tmp;
    const fs::path old_template = tmp.path() / "mount-a" / "cmake" / "module";
    const fs::path new_template = tmp.path() / "mount-b" / "cmake" / "module";
    const fs::path build = tmp.path() / "build" / "main";
    fs::create_directories(old_template);
    fs::create_directories(new_template);
    fs::create_directories(build);
    // No cache yet: a first configure.
    CHECK_FALSE(recomp::configured_for_other_template(build, new_template));
    const auto write_cache = [&](const fs::path& home) {
        std::ofstream(build / "CMakeCache.txt")
            << "# This is the CMakeCache file.\nCMAKE_BUILD_TYPE:STRING=Release\n"
            << "CMAKE_HOME_DIRECTORY:INTERNAL=" << home.generic_string()
            << "\nCMAKE_GENERATOR:INTERNAL=Ninja\n";
    };
    write_cache(new_template);
    CHECK_FALSE(recomp::configured_for_other_template(build, new_template));
    write_cache(old_template);
    CHECK(recomp::configured_for_other_template(build, new_template));
    // The old mount point is gone (an AppImage relaunched).
    fs::remove_all(tmp.path() / "mount-a");
    CHECK(recomp::configured_for_other_template(build, new_template));
}

TEST_CASE("the module build passes the template's cache variables", "[core][recomp][build]") {
    recomp::ModuleBuildOptions o;
    o.module_source_dir = fs::path("prefix") / "cmake" / "module";
    o.abi_include_dir = fs::path("prefix") / "include";
    o.build_dir = fs::path("work") / "build" / "title";
    o.generator_id = "ps2_recomp sha1:0123456789ab";
    o.jobs = 6;
    o.extra_configure_args = {"-DCMAKE_CXX_COMPILER=clang++"};
    recomp::ModuleUnit u;
    u.name = "title";
    u.kind = recomp::UnitKind::overlay;
    u.overlay_id = 4;
    u.profile_id = "TEST_00000";
    u.source_sha1 = std::string(40, 'a');
    u.entry_point = 0;
    u.generated_dir = fs::path("work") / "generated" / "title";
    const auto args = recomp::module_configure_args(o, u);
    auto has = [&](const std::string& a) { return std::find(args.begin(), args.end(), a) != args.end(); };
    auto has_prefix = [&](const std::string& p) {
        return std::any_of(args.begin(), args.end(),
                           [&](const std::string& a) { return a.rfind(p, 0) == 0; });
    };
    CHECK(has("-S"));
    CHECK(has("-B"));
    CHECK(has("Ninja"));
    CHECK(has("-DDQ8_UNIT_NAME=title"));
    CHECK(has("-DDQ8_UNIT_KIND=overlay"));
    CHECK(has("-DDQ8_UNIT_OVERLAY_ID=4"));
    CHECK(has("-DDQ8_UNIT_PROFILE_ID=TEST_00000"));
    CHECK(has("-DDQ8_UNIT_SOURCE_SHA1=" + std::string(40, 'a')));
    CHECK(has("-DDQ8_GENERATOR=ps2_recomp sha1:0123456789ab"));
    CHECK(has("-DDQ8_UNIT_ENTRY_POINT=0x00000000"));
    CHECK(has("-DDQ8_MODULE_PCH=ON"));
    CHECK(has("-DDQ8_MODULE_DEBUG_INFO=OFF"));
    CHECK(has("-DCMAKE_BUILD_TYPE=Release"));
    CHECK(has_prefix("-DDQ8_UNIT_GENERATED_DIR="));
    CHECK(has_prefix("-DDQ8_ABI_INCLUDE_DIR="));
    CHECK(args.back() == "-DCMAKE_CXX_COMPILER=clang++");
    for (const std::string& a : args) {
        CHECK(a.find('\\') == std::string::npos); // CMake gets forward slashes
    }
    const auto build = recomp::module_build_args(o);
    CHECK(build.front() == "--build");
    CHECK(build.back() == "6");
    CHECK_THAT(recomp::module_file_names("title").front(), StartsWith("dq8_unit_title."));
}

TEST_CASE("prepare and generate over unit ELFs are idempotent", "[core][recomp][pipeline]") {
    // Developer mode over a synthetic main unit: our own instructions and addresses.
    namespace m = dq8::test::mips;
    const fs::path dir = fresh_dir("dq8-test-recomp-pipeline");
    m::Program p(0x00100000);
    p.emit(m::lui(m::a0, 0x1234));
    p.emit(m::jr(m::ra));
    p.emit(m::nop());
    p.emit(m::nop());
    dq8::core::ElfWriter writer;
    writer.set_entry(0x00100000);
    dq8::core::ElfSection text;
    text.name = ".text";
    text.flags = dq8::core::elf_shf_alloc | dq8::core::elf_shf_execinstr;
    text.address = 0x00100000;
    text.data = p.bytes();
    writer.add_section(std::move(text));
    writer.write(dir / "in" / "main.elf");
    fs::create_directories(dir / "maps");
    std::ofstream(dir / "maps" / "main.csv") << "Name,Start,End,Size\nstart,0x00100000,0x00100010,16\n";

    const recomp::UnitSource source = recomp::elf_units({{"main", dir / "in" / "main.elf"}}, "dev");
    REQUIRE(source.units.size() == 1);
    CHECK(source.units[0].kind == recomp::UnitKind::main);
    CHECK(source.units[0].entry == 0x00100000);
    CHECK(source.units[0].code_end == 0x00100010);

    recomp::PipelineOptions o;
    o.work_dir = dir / "work";
    o.maps_dir = dir / "maps";
    o.ps2_recomp = DQ8_TEST_CHILD; // plays ps2_recomp (see process_child.cpp)
    const recomp::PrepareReport first = recomp::prepare_units(source, o);
    REQUIRE(first.units.size() == 1);
    CHECK(first.units[0].elf_written);
    CHECK(first.units[0].config_written);
    CHECK(first.units[0].functions == 1);
    CHECK(fs::is_regular_file(dir / "work" / "units" / "units.toml"));
    CHECK(fs::is_regular_file(dir / "work" / ".gitignore"));

    const recomp::PrepareReport again = recomp::prepare_units(source, o);
    CHECK_FALSE(again.units[0].elf_written);
    CHECK_FALSE(again.units[0].config_written);

    const recomp::GenerateReport generated = recomp::generate_units(again, o);
    REQUIRE(generated.units.size() == 1);
    CHECK_FALSE(generated.units[0].skipped);
    CHECK(generated.units[0].result.report.count("functions_recompiled") == 2); // the stand-in's report
    CHECK_THAT(generated.generator_id, StartsWith("ps2_recomp sha1:"));
    const recomp::GenerateReport skipped = recomp::generate_units(again, o);
    CHECK(skipped.units[0].skipped);
    CHECK(skipped.units[0].result.report.count("functions_recompiled") == 2); // from the record

    o.force = true;
    CHECK_FALSE(recomp::generate_units(again, o).units[0].skipped);

    o.units = {"title"};
    CHECK_THROWS_AS(recomp::prepare_units(source, o), Error);
    CHECK_THROWS_AS(recomp::elf_units({{"Main", dir / "in" / "main.elf"}}, "dev"), Error);
    fs::remove_all(dir);
}
