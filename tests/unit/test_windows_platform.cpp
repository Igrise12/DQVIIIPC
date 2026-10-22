// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// What the Windows build relies on beyond the per-file tests (M5 session E): UTF-8 everywhere (the code page
// the application manifest selects, arguments to child processes, file names given as std::string) and game
// modules loaded from a directory whose name is not ASCII, as under many Windows user profiles. The checks
// hold on every platform; on Windows the UTF-8 ones need cmake/toolchains/windows-app.manifest (linked by
// llvm-mingw builds) and skip without it.
#include "dq8/core/process.h"
#include "dq8/core/recomp/module_build.h"
#include "runtime/platform/dynlib.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// Windows builds without the manifest (MSVC, a compile check under ADR-0018) keep the ANSI code page.
#if defined(_WIN32) && !defined(DQ8_TEST_UTF8_MANIFEST)
#define DQ8_SKIP_WITHOUT_UTF8_MANIFEST() SKIP("this Windows build links no UTF-8 application manifest")
#else
#define DQ8_SKIP_WITHOUT_UTF8_MANIFEST() static_cast<void>(0)
#endif

namespace {

// "Dragon Quest – ドラゴンクエスト" in UTF-8 (an en dash and katakana), spelled out so the source stays
// ASCII.
const std::string non_ascii = "Dragon Quest \xE2\x80\x93 \xE3\x83\x89\xE3\x83\xA9\xE3\x82\xB4\xE3\x83\xB3"
                              "\xE3\x82\xAF\xE3\x82\xA8\xE3\x82\xB9\xE3\x83\x88";

fs::path from_utf8(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

struct ScratchDir {
    fs::path dir;
    ScratchDir() {
        dir = fs::temp_directory_path() /
              from_utf8("dq8 " + non_ascii + " " +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

} // namespace

#if defined(_WIN32)
TEST_CASE("windows: the UTF-8 code page is active", "[windows]") {
    DQ8_SKIP_WITHOUT_UTF8_MANIFEST();
    CHECK(GetACP() == CP_UTF8);
}
#endif

TEST_CASE("windows: a non-ASCII argument reaches a child process unchanged", "[windows][process]") {
    DQ8_SKIP_WITHOUT_UTF8_MANIFEST();
    dq8::core::ProcessOptions o;
    o.program = DQ8_TEST_CHILD;
    o.args = {"echo-args", non_ascii};
    o.timeout = 60s;
    const dq8::core::ProcessResult r = dq8::core::run_process(o);
    REQUIRE(r.ok());
    std::string out = r.out;
    std::erase(out, '\r');
    CHECK(out == "[" + non_ascii + "]\n");
}

TEST_CASE("windows: a file named in UTF-8 through std::string round-trips", "[windows][paths]") {
    DQ8_SKIP_WITHOUT_UTF8_MANIFEST();
    const ScratchDir scratch;
    // A narrow string converts through the code page: the same file as the UTF-8 path only when it is UTF-8.
    const fs::path narrow = scratch.dir / fs::path(non_ascii + ".txt");
    {
        std::ofstream out(narrow);
        out << "x";
    }
    CHECK(fs::exists(scratch.dir / from_utf8(non_ascii + ".txt")));
    const fs::directory_entry entry = *fs::directory_iterator(scratch.dir);
    const std::u8string name = entry.path().filename().u8string();
    CHECK(std::string(name.begin(), name.end()) == non_ascii + ".txt");
}

TEST_CASE("windows: a game module loads from a non-ASCII directory", "[windows][dynlib]") {
    const ScratchDir scratch;
    const std::string module = dq8::core::recomp::module_file_names("contract").front();
    const fs::path copy = scratch.dir / module;
    fs::copy_file(fs::path(DQ8_CONTRACT_MODULE_DIR) / module, copy);
    dq8::rt::DynamicLibrary library = dq8::rt::DynamicLibrary::open(copy);
    REQUIRE(library.loaded());
    CHECK(library.symbol("dq8_game_module_describe") != nullptr);
}
