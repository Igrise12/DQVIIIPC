// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The launcher's smaller pieces: error names and texts, the log buffer, the remembered state, finding
// programs and files, the About page's Markdown, and the diagnostics export.
#include "dq8/core/error_text.h"
#include "model/core_backends.h"
#include "model/diagnostics.h"
#include "model/environment.h"
#include "model/errors.h"
#include "model/launcher_state.h"
#include "model/log_buffer.h"
#include "model/markdown.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>

using namespace dq8::launcher;
using dq8::core::ErrorCode;

namespace {

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("launcher: error codes by name and their texts", "[launcher][errors]") {
    for (const ErrorCode code : {ErrorCode::io_open, ErrorCode::unknown_image, ErrorCode::settings_parse,
                                 ErrorCode::setup_cancelled, ErrorCode::registry_parse}) {
        const auto name = dq8::core::to_string(code);
        INFO(name);
        CHECK(error_code_from_name(name) == code);
        const Problem p = explain(code, "detail");
        CHECK(p.code_name == name);
        CHECK(p.message == dq8::core::error_text(code).message);
        CHECK(p.action == dq8::core::error_text(code).action);
        CHECK(p.detail == "detail");
    }
    CHECK_FALSE(error_code_from_name(""));
    CHECK_FALSE(error_code_from_name("unknown"));
    CHECK_FALSE(error_code_from_name("no-such-code"));

    const Problem named = explain_name("toolchain-incomplete", "cmake 3.20 is too old");
    REQUIRE(named.code);
    CHECK(named.detail == "cmake 3.20 is too old");
    const Problem other = explain_name("audio-underrun", "the buffer ran dry");
    CHECK_FALSE(other.code);
    CHECK(other.message == "the buffer ran dry");

    const Problem from_error = explain_exception(dq8::core::Error(ErrorCode::io_write, "cannot write x"));
    CHECK(*from_error.code == ErrorCode::io_write);
    const Problem from_logic = explain_exception(std::logic_error("not implemented"));
    CHECK_FALSE(from_logic.code);
    CHECK(from_logic.message == "not implemented");
}

TEST_CASE("launcher: the log buffer keeps the last lines", "[launcher][log]") {
    LogBuffer log(3);
    log.add("one\r\n");
    log.add("two\nthree");
    log.add("four");
    CHECK(log.lines() == std::deque<std::string>{"two", "three", "four"});
    CHECK(log.dropped() == 1);
    CHECK(log.text() == "two\nthree\nfour\n");
    const auto g = log.generation();
    log.clear();
    CHECK(log.lines().empty());
    CHECK(log.generation() > g);
}

TEST_CASE("launcher: the remembered state round-trips and never fails", "[launcher][state]") {
    LauncherState s;
    s.last_image = std::filesystem::path(u8"/discs/dq8 é.iso");
    s.screen = "settings";
    s.show_advanced = true;
    CHECK(parse_launcher_state(format_launcher_state(s)) == s);
    CHECK(parse_launcher_state("") == LauncherState{});
    CHECK(parse_launcher_state("[1,2]") == LauncherState{});
    CHECK(parse_launcher_state(R"({"screen":3,"show_advanced":"yes"})") == LauncherState{});

    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "dq8-launcher-state-test" / "launcher.json";
    std::error_code ec;
    std::filesystem::remove_all(file.parent_path(), ec);
    CHECK(load_launcher_state(file) == LauncherState{}); // missing
    save_launcher_state(file, s);
    CHECK(load_launcher_state(file) == s);
    std::filesystem::remove_all(file.parent_path(), ec);
    CHECK(load_launcher_state({}) == LauncherState{});
}

TEST_CASE("launcher: UTF-8 paths", "[launcher][environment]") {
    const std::string text = "/discs/\xc3\xa9t\xc3\xa9.iso";
    CHECK(to_utf8(from_utf8(text)) == text);
}

TEST_CASE("launcher: programs and files are found in a build tree and an install",
          "[launcher][environment]") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "dq8-launcher-env-test";
    std::error_code ec;
    fs::remove_all(root, ec);
#ifdef _WIN32
    const std::string exe = ".exe";
#else
    const std::string exe;
#endif
    // A build tree: src/launcher, src/host/dq8-game, src/cli/dq8ctl.
    fs::create_directories(root / "build" / "src" / "launcher");
    fs::create_directories(root / "build" / "src" / "host");
    fs::create_directories(root / "build" / "src" / "cli");
    std::ofstream(root / "build" / "src" / "host" / ("dq8-game" + exe)) << "x";
    std::ofstream(root / "build" / "src" / "cli" / ("dq8ctl" + exe)) << "x";
    const fs::path launcher_dir = root / "build" / "src" / "launcher";
    CHECK(locate_program(launcher_dir, "dq8-game", "host") ==
          root / "build" / "src" / "host" / ("dq8-game" + exe));
    const Environment env = discover_environment(launcher_dir);
    CHECK(env.game == root / "build" / "src" / "host" / ("dq8-game" + exe));
    CHECK(env.ctl == root / "build" / "src" / "cli" / ("dq8ctl" + exe));
    const Environment chosen = discover_environment(launcher_dir, "/x/game", "/x/ctl");
    CHECK(chosen.game == "/x/game");
    CHECK(chosen.ctl == "/x/ctl");

    // An install: bin/ with the programs, share/ beside it.
    fs::create_directories(root / "install" / "bin");
    fs::create_directories(root / "install" / "share" / "doc" / "dq8-native");
    fs::create_directories(root / "install" / "share" / "dq8" / "regions");
    std::ofstream(root / "install" / "bin" / ("dq8-game" + exe)) << "x";
    std::ofstream(root / "install" / "share" / "doc" / "dq8-native" / "THIRD_PARTY_NOTICES.md") << "# x\n";
    const fs::path bin = root / "install" / "bin";
    CHECK(locate_program(bin, "dq8-game", "host") == bin / ("dq8-game" + exe));
    CHECK(find_shared_file(bin, "THIRD_PARTY_NOTICES.md",
                           fs::path("share") / "doc" / "dq8-native" / "THIRD_PARTY_NOTICES.md") ==
          root / "install" / "share" / "doc" / "dq8-native" / "THIRD_PARTY_NOTICES.md");
    CHECK(find_regions_dir(bin) == root / "install" / "share" / "dq8" / "regions");
    fs::remove_all(root, ec);

    // The source tree's own copy, from the build directory.
    CHECK(find_shared_file(fs::path(DQ8_SOURCE_DIR) / "build" / "x" / "src" / "launcher",
                           "THIRD_PARTY_NOTICES.md", "share/doc/dq8-native/THIRD_PARTY_NOTICES.md"));
}

TEST_CASE("launcher: Markdown for the notices", "[launcher][about]") {
    const auto blocks = parse_markdown(
        "# Title\n\nSome text\ncontinues here.\n\n| A | B |\n|---|---|\n| [x](https://e.org) | "
        "`y` |\n| p \\| q | **z** |\n\n- item one\n  more\n- item two\n\n---\n## Sub\n");
    REQUIRE(blocks.size() == 7);
    CHECK(blocks[0].kind == MdBlock::Kind::heading);
    CHECK(blocks[0].level == 1);
    CHECK(blocks[0].text == "Title");
    CHECK(blocks[1].kind == MdBlock::Kind::paragraph);
    CHECK(blocks[1].text == "Some text continues here.");
    REQUIRE(blocks[2].kind == MdBlock::Kind::table);
    REQUIRE(blocks[2].rows.size() == 3);
    CHECK(blocks[2].rows[0] == std::vector<std::string>{"A", "B"});
    CHECK(blocks[2].rows[1] == std::vector<std::string>{"x <https://e.org>", "y"});
    CHECK(blocks[2].rows[2] == std::vector<std::string>{"p | q", "z"});
    CHECK(blocks[3].kind == MdBlock::Kind::bullet);
    CHECK(blocks[3].text == "item one more");
    CHECK(blocks[4].text == "item two");
    CHECK(blocks[5].kind == MdBlock::Kind::rule);
    CHECK(blocks[6].level == 2);

    // The real notices parse into headings and tables.
    const auto notices =
        parse_markdown(read_file(std::filesystem::path(DQ8_SOURCE_DIR) / "THIRD_PARTY_NOTICES.md"));
    std::size_t tables = 0;
    for (const MdBlock& b : notices) {
        if (b.kind == MdBlock::Kind::table) {
            ++tables;
            CHECK(b.rows.size() >= 2);
        }
    }
    CHECK(tables >= 3);
}

namespace {

class FakeExporter final : public DiagnosticsExporter {
  public:
    bool ok = true;
    std::filesystem::path written;
    ExportOutcome write(const std::filesystem::path& file, const std::atomic<bool>& /*cancel*/) override {
        written = file;
        ExportOutcome out;
        out.ok = ok;
        out.output = "collected 5 sections";
        if (!ok) {
            out.problem = explain(ErrorCode::io_write, "disk full");
        }
        return out;
    }
};

} // namespace

TEST_CASE("launcher: the diagnostics export", "[launcher][diagnostics]") {
    auto exporter = std::make_unique<FakeExporter>();
    FakeExporter* fake = exporter.get();
    DiagnosticsModel m(std::move(exporter), inline_executors());
    CHECK(m.state() == ExportState::idle);
    CHECK_FALSE(m.start({}));
    REQUIRE(m.start("/tmp/report.txt"));
    m.pump();
    CHECK(m.state() == ExportState::done);
    CHECK(fake->written == "/tmp/report.txt");
    CHECK(m.log().text().find("collected 5 sections") != std::string::npos);

    fake->ok = false;
    REQUIRE(m.start("/tmp/report2.txt"));
    m.pump();
    CHECK(m.state() == ExportState::failed);
    REQUIRE(m.problem());
    CHECK(*m.problem()->code == ErrorCode::io_write);
}

TEST_CASE("launcher: dq8ctl diagnose is called with --out", "[launcher][diagnostics]") {
    std::vector<std::string> args;
    std::filesystem::path program;
    auto exporter = ctl_diagnostics("/opt/dq8/bin/dq8ctl", [&](const dq8::core::ProcessOptions& o) {
        program = o.program;
        args = o.args;
        dq8::core::ProcessResult r;
        r.exit_code = args.size() == 3 ? 0 : 2;
        r.out = "written\n";
        return r;
    });
    std::atomic<bool> cancel{false};
    const ExportOutcome out = exporter->write("/tmp/d.txt", cancel);
    CHECK(out.ok);
    CHECK(program == "/opt/dq8/bin/dq8ctl");
    CHECK(args == std::vector<std::string>{"diagnose", "--out", "/tmp/d.txt"});
    CHECK(out.output == "written\n");

    auto failing = ctl_diagnostics("/opt/dq8/bin/dq8ctl", [](const dq8::core::ProcessOptions&) {
        dq8::core::ProcessResult r;
        r.exit_code = 2;
        r.err = "dq8ctl: unknown command 'diagnose'\n";
        return r;
    });
    const ExportOutcome bad = failing->write("/tmp/d.txt", cancel);
    CHECK_FALSE(bad.ok);
    REQUIRE(bad.problem);
    CHECK(bad.problem->message.find("status 2") != std::string::npos);
    CHECK(bad.output.find("unknown command") != std::string::npos);

    const ExportOutcome none = ctl_diagnostics({})->write("/tmp/d.txt", cancel);
    CHECK_FALSE(none.ok);
    CHECK(*none.problem->code == ErrorCode::tool_missing);
}

TEST_CASE("launcher: the diagnostics file name", "[launcher][diagnostics]") {
    const std::string name = default_diagnostics_name(std::time_t{0});
    CHECK(name.rfind("dq8-diagnostics-19", 0) == 0); // 1970, or 1969 west of Greenwich
    CHECK(name.size() == std::string("dq8-diagnostics-19700101-000000.txt").size());
}
