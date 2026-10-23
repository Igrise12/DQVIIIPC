// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// M5 session A: setup::run() on a synthetic disc (dq8/core/setup.h): the stage events in order, the log,
// resumption, cancellation and the free-space check. The full chain to a built module needs the disc and
// ps2_recomp (Docs/milestones/M5-launcher.md, "Protocol notes").
#include "dq8/core/diagnostics.h"
#include "dq8/core/hash.h"
#include "dq8/core/setup.h"
#include "dq8/core/toolchain.h"
#include "support/iso_builder.h"
#include "support/mwo3_builder.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using dq8::core::ErrorCode;
namespace setup = dq8::core::setup;
namespace fs = std::filesystem;

namespace {

std::string sized(const std::vector<std::uint8_t>& data) {
    return "size = " + std::to_string(data.size()) + "\nsha1 = \"" +
           dq8::core::to_hex(dq8::core::Sha1::of(data)) + "\"\ncrc32 = \"" +
           dq8::core::to_hex(dq8::core::Crc32::of(data)) + "\"\n";
}

// A synthetic disc (a fake boot ELF, one overlay, one module) and its region profile, as in test_intake.cpp.
struct Disc {
    dq8::test::TempDir tmp;
    fs::path iso = tmp.path() / "synthetic.iso";
    fs::path regions = tmp.path() / "regions";
    fs::path maps = tmp.path() / "maps";
    fs::path work = tmp.path() / "work";

    Disc() {
        dq8::test::Mwo3Spec spec;
        spec.overlay_id = 1;
        spec.load_address = 0x00200000;
        spec.text_size = 0x200;
        spec.data_size = 0x80;
        spec.bss_size = 0x40;
        spec.table_offset = 0x10;
        spec.table_entries = 2;
        spec.name = "a";
        spec.seed = 21;
        const std::vector<std::uint8_t> overlay = dq8::test::build_mwo3(spec);
        const std::vector<std::uint8_t> boot =
            dq8::test::fake_boot_elf(0x00100008, 0x00100000, 0x4000, 0x00200000, {0x300}, 9000, 77);
        const std::vector<std::uint8_t> module = dq8::test::seeded_bytes(23, 4097);
        dq8::test::IsoBuilder b;
        b.set_system_identifier("PLAYSTATION");
        const std::string cnf = "BOOT2 = cdrom0:\\BOOT.ELF;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";
        b.add_file("/SYSTEM.CNF", std::vector<std::uint8_t>(cnf.begin(), cnf.end()));
        b.add_file("/BOOT.ELF", boot);
        b.add_file("/BIN/A.BIN", overlay);
        b.add_file("/MODULES/X.IRX", module);
        b.write(iso);
        fs::create_directories(regions);
        fs::create_directories(maps);
        std::ofstream(regions / "TEST_00001.toml", std::ios::binary)
            << "schema_version = 1\n[profile]\nid = \"TEST_00001\"\nserial = \"TEST-00001\"\n"
               "title = \"Synthetic\"\nregion = \"NTSC-U\"\nvideo_mode = \"NTSC\"\n"
               "[image]\nsize = "
            << fs::file_size(iso) << "\nsector_size = 2048\n[boot]\npath = \"BOOT.ELF\"\n"
            << sized(boot)
            << "entry = 0x00100008\nload_address = 0x00100000\nmain_memsz = 0x4000\n"
               "[overlay_window]\nbase = 0x00200000\nend = 0x00210000\ncontainer = \"MWo3\"\nheader_size = "
               "64\n"
               "code_offset = 64\nwindow_pointer = 0x00100100\nloader = 0x00100200\nloader_hook = "
               "0x00100210\n"
               "load_by_index = 0x00100300\nname_table = 0x00100400\nname_table_order = [\"A\"]\n"
               "lsn_cache = 0x00100500\nctor_walker = 0x00100600\nflush_cache_stub = 0x00100700\n"
               "mode_variable = 0x00100800\n"
               "[[overlays]]\nname = \"A\"\nid = 1\nindex = 0\nsegment_index = 1\npath = \"BIN/A.BIN\"\n"
               "text_size = 0x200\ndata_size = 0x80\nbss_size = 0x40\ntable_entries = 2\nsegment_memsz = "
               "0x300\n"
            << sized(overlay) << "[[files]]\npath = \"MODULES/X.IRX\"\nrole = \"iop_module\"\n"
            << sized(module);
    }

    setup::Options options(std::vector<setup::Event>& events) const {
        setup::Options o;
        o.iso = iso;
        o.work_dir = work;
        o.regions_dir = regions;
        o.maps_dir = maps; // contributor maps, empty: the setup stops at the maps stage
        o.executable_dir = tmp.path();
        o.ps2_recomp = DQ8_TEST_CHILD; // any program: the stages that run it are not reached
        o.game_executable = DQ8_TEST_CHILD;
        o.module_template_dir = fs::path(DQ8_SOURCE_DIR) / "cmake" / "module";
        o.abi_include_dir = fs::path(DQ8_SOURCE_DIR) / "include";
        o.registry_file = tmp.path() / "installs.toml";
        o.required_free_bytes = 1;
        o.on_event = [&events](const setup::Event& e) { events.push_back(e); };
        return o;
    }
};

// "stage:state" for every event but progress.
std::vector<std::string> outline(const std::vector<setup::Event>& events) {
    std::vector<std::string> out;
    for (const setup::Event& e : events) {
        if (e.state != setup::State::progress) {
            out.push_back(std::string(setup::to_string(e.stage)) + ":" +
                          std::string(setup::to_string(e.state)));
        }
    }
    return out;
}

bool toolchain_complete() {
    return dq8::core::toolchain::check(dq8::core::toolchain::detect({})).empty();
}

} // namespace

TEST_CASE("setup: stages report in order, a missing map stops the run, a second run skips the extract",
          "[m5][setup]") {
    const Disc disc;
    std::vector<setup::Event> events;
    const setup::Result r = setup::run(disc.options(events));
    CHECK_FALSE(r.ok);
    const bool complete = toolchain_complete();
    const std::vector<std::string> expected_tail =
        complete ? std::vector<std::string>{"toolchain:start", "toolchain:done", "maps:start", "maps:error"}
                 : std::vector<std::string>{"toolchain:start", "toolchain:error"};
    std::vector<std::string> expected{"verify:start", "verify:done", "extract:start", "extract:done"};
    expected.insert(expected.end(), expected_tail.begin(), expected_tail.end());
    CHECK(outline(events) == expected);
    REQUIRE(r.code);
    CHECK(*r.code == (complete ? ErrorCode::maps_missing : ErrorCode::toolchain_incomplete));
    CHECK(events.back().code == r.code);
    if (complete) {
        CHECK_THAT(r.message, ContainsSubstring("lack main, a"));
    }
    // Byte progress for the extract stage, against the profile's sizes.
    bool extract_progress = false;
    for (const setup::Event& e : events) {
        extract_progress =
            extract_progress || (e.stage == setup::Stage::extract && e.state == setup::State::progress &&
                                 e.total != 0 && e.done == e.total);
    }
    CHECK(extract_progress);
    CHECK(r.work_dir == fs::absolute(disc.work));
    REQUIRE(fs::exists(r.log_file));
    std::stringstream log;
    log << std::ifstream(r.log_file).rdbuf();
    CHECK_THAT(log.str(), ContainsSubstring("verify done: TEST_00001 (Synthetic)"));
    CHECK_THAT(log.str(), ContainsSubstring(complete ? "maps error [maps-missing]" : "toolchain error"));

    // The files are there now: the extract is up to date, and the previous log is kept beside the new one.
    events.clear();
    (void)setup::run(disc.options(events));
    const std::vector<std::string> again = outline(events);
    REQUIRE(again.size() >= 4);
    CHECK(again[3] == "extract:skipped");
    CHECK(fs::exists(disc.work / "logs" / "setup.previous.log"));

    // The diagnostics report shows the setup log's event lines, never the continuation lines.
    dq8::core::diagnostics::Options d;
    d.work_dir = disc.work;
    d.registry_file = disc.tmp.path() / "installs.toml";
    d.executable_dir = fs::path(DQ8_CONTRACT_MODULE_DIR);
    d.settings_file = disc.tmp.path() / "settings.toml";
    const std::string report = dq8::core::diagnostics::report(d);
    CHECK_THAT(report, ContainsSubstring("[system]"));
    CHECK_THAT(report, ContainsSubstring("[toolchain]"));
    CHECK_THAT(report, ContainsSubstring("display.window_mode = windowed (defaults)"));
    CHECK_THAT(report, ContainsSubstring("no game is set up yet"));
    CHECK_THAT(report, ContainsSubstring("extract skipped"));
    CHECK(report.find("\n  | ") == std::string::npos);
}

TEST_CASE("setup: a cancel ends the stage that is running with setup-cancelled", "[m5][setup]") {
    const Disc disc;
    std::vector<setup::Event> events;
    std::atomic<bool> cancel{true};
    setup::Options o = disc.options(events);
    o.cancel = &cancel;
    const setup::Result r = setup::run(o);
    CHECK_FALSE(r.ok);
    REQUIRE(r.code);
    CHECK(*r.code == ErrorCode::setup_cancelled);
    CHECK(outline(events) == std::vector<std::string>{"verify:start", "verify:error"});
    CHECK(dq8::core::to_string(*events.back().code) == "setup-cancelled");

    // A later run resumes (and here gets as far as the first run of the test above).
    cancel = false;
    events.clear();
    (void)setup::run(o);
    CHECK(outline(events).size() >= 4);
}

TEST_CASE("setup: too little free space stops the run before anything is written", "[m5][setup]") {
    const Disc disc;
    std::vector<setup::Event> events;
    setup::Options o = disc.options(events);
    o.required_free_bytes = std::uint64_t{1} << 62;
    const setup::Result r = setup::run(o);
    REQUIRE(r.code);
    CHECK(*r.code == ErrorCode::insufficient_space);
    CHECK(outline(events) == std::vector<std::string>{"verify:start", "verify:error"});
    CHECK_THAT(r.message, ContainsSubstring("the setup needs about"));
    CHECK_FALSE(fs::exists(disc.work / "baserom"));
}

TEST_CASE("setup: a disc no profile knows ends the verify stage with its code", "[m5][setup]") {
    const Disc disc;
    std::vector<setup::Event> events;
    setup::Options o = disc.options(events);
    o.iso = disc.tmp.path() / "missing.iso";
    const setup::Result r = setup::run(o);
    CHECK_FALSE(r.ok);
    REQUIRE(r.code);
    CHECK(outline(events) == std::vector<std::string>{"verify:start", "verify:error"});
    CHECK(events.back().code == r.code);
    CHECK(setup::to_json_line(events.back()).find(R"("state":"error")") != std::string::npos);
}
