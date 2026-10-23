// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// The bundled toolchain's download (src/core/toolchain/download.h, M5 session E): the pin, consent, file://
// and a local fake HTTP server, resume, cancel, the checksum, the free-space check and the unpack through
// cmake. The archives are small zips made here with `cmake -E tar`; nothing is fetched from the network.
#include "core/toolchain/download.h"
#include "dq8/core/error.h"
#include "dq8/core/process.h"
#include "support/fake_http_server.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using dq8::core::Error;
using dq8::core::ErrorCode;
using namespace dq8::core::toolchain;
using dq8::test::FakeHttpServer;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::string read_all(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void write_all(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

ErrorCode code_of(const std::function<void()>& f) {
    try {
        f();
    } catch (const Error& e) {
        return e.code();
    }
    FAIL_CHECK("no dq8::core::Error was thrown"); // not FAIL: MSVC flags the return after it as unreachable
    return ErrorCode::io_open;
}

// The cmake that unpacks the archives: the build's own on a native build, a Windows cmake.exe when cross
// compiled (m5_windows_tests.cmake). Empty when there is none: the tests that unpack are skipped.
std::optional<fs::path> test_cmake() {
    const fs::path cmake = DQ8_TEST_CMAKE;
    if (cmake.empty() || !fs::is_regular_file(cmake)) {
        return std::nullopt;
    }
    return cmake;
}

// A temporary directory with a space in its name (paths and file:// URLs must survive it), removed
// afterwards.
struct Scratch {
    fs::path dir;
    explicit Scratch(const std::string& name) {
        dir = fs::temp_directory_path() /
              ("dq8 download " + name + " " +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

// A toolchain-shaped zip: <root>/bin/tool.txt and 600 KiB of incompressible bytes, so a copy takes many
// chunks.
struct Archive {
    fs::path file;
    std::string root = "fake-toolchain";
    std::uint64_t size = 0;
    std::string sha256;
};

Archive make_archive(const fs::path& cmake, const fs::path& dir) {
    Archive a;
    const fs::path source = dir / "source";
    write_all(source / a.root / "bin" / "tool.txt", "a tool\n");
    std::string noise(600u << 10, '\0');
    std::uint32_t x = 2463534242u;
    for (char& c : noise) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        c = static_cast<char>(x & 0xFF);
    }
    write_all(source / a.root / "lib" / "noise.bin", noise);
    a.file = dir / "toolchain-archive.zip";
    dq8::core::ProcessOptions p;
    p.program = cmake;
    p.args = {"-E", "tar", "cf", a.file.string(), "--format=zip", a.root};
    p.working_dir = source;
    p.timeout = 120s;
    const dq8::core::ProcessResult r = dq8::core::run_process(p);
    INFO(r.out << r.err);
    REQUIRE(r.ok());
    a.size = fs::file_size(a.file);
    a.sha256 = sha256_file(a.file);
    return a;
}

DownloadOptions options_for(const Archive& a, const fs::path& cmake, const fs::path& root, std::string url) {
    DownloadOptions o;
    o.consent = true;
    o.bundled_root = root;
    o.cmake = cmake;
    o.pin.version = "testver";
    o.pin.url = std::move(url);
    o.pin.sha256 = a.sha256;
    o.pin.size = a.size;
    o.pin.archive_root = a.root;
    o.pin.unpacked_bytes = 1u << 20;
    o.pin.entries = 5;
    return o;
}

void check_installed(const DownloadResult& r, const fs::path& root) {
    CHECK(r.root == root / "testver");
    CHECK(read_all(r.root / "bin" / "tool.txt") == "a tool\n");
    CHECK(fs::file_size(r.root / "lib" / "noise.bin") == (600u << 10));
    CHECK(fs::exists(r.root / ".dq8-toolchain"));
    CHECK_FALSE(fs::exists(root / "testver.unpack"));
    CHECK(fs::is_empty(root / "downloads")); // the archive is deleted after unpacking
}

} // namespace

TEST_CASE("toolchain download: SHA-256 known answers", "[toolchain][download]") {
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::string million(1000000, 'a');
    CHECK(sha256_hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // Odd-sized updates give the same digest as one update.
    Sha256 h;
    for (std::size_t at = 0; at < million.size();) {
        const std::size_t n = std::min<std::size_t>(million.size() - at, 1 + at % 131);
        h.update(million.data() + at, n);
        at += n;
    }
    const std::array<std::uint8_t, 32> digest = h.finish();
    CHECK(digest[0] == 0xcd);
    CHECK(digest[31] == 0xd0);
}

TEST_CASE("toolchain download: the pin matches cmake/toolchains/llvm-mingw-pin.cmake",
          "[toolchain][download]") {
    const std::string cmake =
        read_all(fs::path(DQ8_SOURCE_DIR) / "cmake" / "toolchains" / "llvm-mingw-pin.cmake");
    REQUIRE_FALSE(cmake.empty());
    const auto value = [&](const std::string& name) {
        std::smatch m;
        const std::regex re("set\\(" + name + "\\s+\"([^\"]*)\"\\)");
        REQUIRE(std::regex_search(cmake, m, re));
        return m[1].str();
    };
    const DownloadPin& pin = pinned_llvm_mingw();
    const std::string version = value("DQ8_LLVM_MINGW_VERSION");
    const std::string archive = std::regex_replace(value("DQ8_LLVM_MINGW_WINDOWS_ARCHIVE"),
                                                   std::regex("\\$\\{DQ8_LLVM_MINGW_VERSION\\}"), version);
    CHECK(pin.version == version);
    CHECK(pin.url == "https://github.com/mstorsjo/llvm-mingw/releases/download/" + version + "/" + archive);
    CHECK(pin.sha256 == value("DQ8_LLVM_MINGW_WINDOWS_SHA256"));
    CHECK(std::to_string(pin.size) == value("DQ8_LLVM_MINGW_WINDOWS_SIZE"));
    CHECK(pin.archive_root + ".zip" == archive);
    CHECK(pin.unpacked_bytes > pin.size);
    CHECK(pin.entries > 0);
}

TEST_CASE("toolchain download: file URLs", "[toolchain][download]") {
    const std::string url = file_url(fs::temp_directory_path() / "a b" / "c%d.zip");
    CHECK(url.rfind("file:///", 0) == 0);
    CHECK_THAT(url, ContainsSubstring("/a%20b/c%25d.zip"));
}

TEST_CASE("toolchain download: nothing happens without consent", "[toolchain][download]") {
    Scratch scratch("consent");
    DownloadOptions o;
    o.bundled_root = scratch.dir / "toolchain";
    o.pin.url = file_url(scratch.dir / "missing.zip");
    CHECK(code_of([&] { (void)download_toolchain(o); }) == ErrorCode::download_failed);
    CHECK_FALSE(fs::exists(scratch.dir / "toolchain" / "downloads"));

    // An installed toolchain is found without consent (and without a download).
    write_all(o.bundled_root / o.pin.version / ".dq8-toolchain", o.pin.version + " " + o.pin.sha256 + "\n");
    const DownloadResult r = download_toolchain(o);
    CHECK(r.already_installed);
    CHECK(r.root == o.bundled_root / o.pin.version);
    REQUIRE(installed_toolchain(o.bundled_root, o.pin).has_value());

    // A marker for another digest is not this toolchain.
    write_all(o.bundled_root / o.pin.version / ".dq8-toolchain",
              o.pin.version + " " + std::string(64, '0') + "\n");
    CHECK_FALSE(installed_toolchain(o.bundled_root, o.pin).has_value());
}

TEST_CASE("toolchain download: a malformed pin is refused", "[toolchain][download]") {
    DownloadOptions o;
    o.consent = true;
    o.bundled_root = fs::temp_directory_path() / "dq8-never-created";
    o.pin.version = "../escape";
    CHECK(code_of([&] { (void)download_toolchain(o); }) == ErrorCode::invalid_argument);
    o.pin = pinned_llvm_mingw();
    o.pin.sha256 = "ABC";
    CHECK(code_of([&] { (void)download_toolchain(o); }) == ErrorCode::invalid_argument);
    o.pin = pinned_llvm_mingw();
    o.pin.url = "ftp://example.invalid/x.zip";
    CHECK(code_of([&] { (void)download_toolchain(o); }) == ErrorCode::invalid_argument);
    CHECK_FALSE(fs::exists(o.bundled_root));
}

TEST_CASE("toolchain download: from a file URL, with progress, then idempotent", "[toolchain][download]") {
    const std::optional<fs::path> cmake = test_cmake();
    if (!cmake) {
        SKIP("no cmake for this platform (DQ8_TEST_WINDOWS_CMAKE)");
    }
    Scratch scratch("file");
    const Archive a = make_archive(*cmake, scratch.dir);
    const fs::path root = scratch.dir / "toolchain";
    DownloadOptions o = options_for(a, *cmake, root, file_url(a.file));

    std::vector<DownloadProgress> seen;
    o.on_progress = [&](const DownloadProgress& p) { seen.push_back(p); };
    const DownloadResult r = download_toolchain(o);
    check_installed(r, root);
    CHECK_FALSE(r.already_installed);
    CHECK(r.resumed_from == 0);
    CHECK(r.downloaded_bytes == a.size);

    // Phases in order, each monotonic, ending at its total.
    REQUIRE_FALSE(seen.empty());
    DownloadPhase phase = DownloadPhase::download;
    std::uint64_t last = 0;
    for (const DownloadProgress& p : seen) {
        if (p.phase != phase) {
            CHECK(static_cast<int>(p.phase) > static_cast<int>(phase));
            phase = p.phase;
            last = 0;
        }
        CHECK(p.done >= last);
        if (p.total != 0) {
            CHECK(p.done <= p.total);
        }
        last = p.done;
    }
    CHECK(phase == DownloadPhase::unpack);
    const auto final_of = [&](DownloadPhase ph) {
        std::optional<DownloadProgress> f;
        for (const DownloadProgress& p : seen) {
            if (p.phase == ph) {
                f = p;
            }
        }
        return f;
    };
    REQUIRE(final_of(DownloadPhase::download));
    CHECK(final_of(DownloadPhase::download)->done == a.size);
    REQUIRE(final_of(DownloadPhase::verify));
    CHECK(final_of(DownloadPhase::verify)->done == a.size);
    REQUIRE(final_of(DownloadPhase::unpack));
    CHECK(final_of(DownloadPhase::unpack)->done == 5); // three directories, two files

    // Again: already installed, nothing transferred, no consent needed.
    o.consent = false;
    const DownloadResult again = download_toolchain(o);
    CHECK(again.already_installed);
    CHECK(again.downloaded_bytes == 0);
}

TEST_CASE("toolchain download: resumes a partial download", "[toolchain][download]") {
    const std::optional<fs::path> cmake = test_cmake();
    if (!cmake) {
        SKIP("no cmake for this platform (DQ8_TEST_WINDOWS_CMAKE)");
    }
    Scratch scratch("resume");
    const Archive a = make_archive(*cmake, scratch.dir);
    const fs::path root = scratch.dir / "toolchain";
    const std::string bytes = read_all(a.file);
    write_all(root / "downloads" / "toolchain-archive.zip.part", bytes.substr(0, 100000));

    const DownloadResult r = download_toolchain(options_for(a, *cmake, root, file_url(a.file)));
    check_installed(r, root);
    CHECK(r.resumed_from == 100000);
    CHECK(r.downloaded_bytes == a.size - 100000);
}

TEST_CASE("toolchain download: cancel keeps the partial file, the next call resumes",
          "[toolchain][download]") {
    const std::optional<fs::path> cmake = test_cmake();
    if (!cmake) {
        SKIP("no cmake for this platform (DQ8_TEST_WINDOWS_CMAKE)");
    }
    Scratch scratch("cancel");
    const Archive a = make_archive(*cmake, scratch.dir);
    const fs::path root = scratch.dir / "toolchain";
    DownloadOptions o = options_for(a, *cmake, root, file_url(a.file));
    std::atomic<bool> cancel{false};
    o.cancel = &cancel;
    o.on_progress = [&](const DownloadProgress& p) {
        if (p.phase == DownloadPhase::download && p.done > 0) {
            cancel = true;
        }
    };
    CHECK(code_of([&] { (void)download_toolchain(o); }) == ErrorCode::setup_cancelled);
    const fs::path part = root / "downloads" / "toolchain-archive.zip.part";
    REQUIRE(fs::exists(part));
    const std::uint64_t kept = fs::file_size(part);
    CHECK(kept > 0);
    CHECK(kept < a.size);
    CHECK_FALSE(installed_toolchain(root, o.pin).has_value());

    cancel = false;
    o.on_progress = {};
    const DownloadResult r = download_toolchain(o);
    check_installed(r, root);
    CHECK(r.resumed_from == kept);
}

TEST_CASE("toolchain download: a checksum mismatch deletes the download", "[toolchain][download]") {
    const std::optional<fs::path> cmake = test_cmake();
    if (!cmake) {
        SKIP("no cmake for this platform (DQ8_TEST_WINDOWS_CMAKE)");
    }
    Scratch scratch("checksum");
    const Archive a = make_archive(*cmake, scratch.dir);
    const fs::path root = scratch.dir / "toolchain";
    DownloadOptions o = options_for(a, *cmake, root, file_url(a.file));
    o.pin.sha256 = std::string(64, 'f');
    try {
        (void)download_toolchain(o);
        FAIL("the download passed a wrong checksum");
    } catch (const Error& e) {
        CHECK(e.code() == ErrorCode::download_failed);
        CHECK_THAT(e.what(), ContainsSubstring(a.sha256));
    }
    CHECK(fs::is_empty(root / "downloads"));
    CHECK_FALSE(fs::exists(root / "testver"));
}

TEST_CASE("toolchain download: free space, archive layout and leftovers", "[toolchain][download]") {
    const std::optional<fs::path> cmake = test_cmake();
    if (!cmake) {
        SKIP("no cmake for this platform (DQ8_TEST_WINDOWS_CMAKE)");
    }
    Scratch scratch("layout");
    const Archive a = make_archive(*cmake, scratch.dir);
    const fs::path root = scratch.dir / "toolchain";

    DownloadOptions huge = options_for(a, *cmake, root, file_url(a.file));
    huge.pin.unpacked_bytes = std::uint64_t{1} << 60;
    CHECK(code_of([&] { (void)download_toolchain(huge); }) == ErrorCode::insufficient_space);

    DownloadOptions wrong_root = options_for(a, *cmake, root, file_url(a.file));
    wrong_root.keep_archive = true;
    wrong_root.pin.archive_root = "another-root";
    CHECK(code_of([&] { (void)download_toolchain(wrong_root); }) == ErrorCode::download_failed);
    CHECK_FALSE(fs::exists(root / "testver.unpack"));
    CHECK(fs::exists(root / "downloads" / "toolchain-archive.zip")); // verified, kept for the next attempt

    // An interrupted earlier unpack (no marker) is replaced; the verified archive is reused.
    write_all(root / "testver" / "stale.txt", "left over\n");
    const DownloadResult r = download_toolchain(options_for(a, *cmake, root, file_url(a.file)));
    check_installed(r, root);
    CHECK(r.downloaded_bytes == 0);
    CHECK_FALSE(fs::exists(root / "testver" / "stale.txt"));
}

TEST_CASE("toolchain download: over HTTP from a local server", "[toolchain][download][http]") {
    const std::optional<fs::path> cmake = test_cmake();
    if (!cmake) {
        SKIP("no cmake for this platform (DQ8_TEST_WINDOWS_CMAKE)");
    }
#if !defined(_WIN32)
    if (!dq8::core::find_program("curl")) {
        SKIP("curl is not installed (the HTTP transport on this system)");
    }
#endif
    Scratch scratch("http");
    const Archive a = make_archive(*cmake, scratch.dir);
    const std::string body = read_all(a.file);
    const fs::path root = scratch.dir / "toolchain";

    SECTION("a plain download, through a redirect") {
        FakeHttpServer::Behaviour b;
        b.redirect_from = "/latest.zip";
        FakeHttpServer server("/files/toolchain.zip", body, b);
        const DownloadResult r = download_toolchain(options_for(a, *cmake, root, server.url("/latest.zip")));
        CHECK(r.root == root / "testver");
        CHECK(read_all(r.root / "bin" / "tool.txt") == "a tool\n");
        const std::vector<FakeHttpServer::Request> seen = server.requests();
        REQUIRE(seen.size() == 2);
        CHECK(seen[0].status == 302);
        CHECK(seen[1].status == 200);
    }
    SECTION("a dropped connection is resumed with a range") {
        FakeHttpServer::Behaviour b;
        b.drop_after = 200000;
        FakeHttpServer server("/toolchain.zip", body, b);
        const DownloadResult r = download_toolchain(options_for(a, *cmake, root, server.url()));
        CHECK(read_all(r.root / "bin" / "tool.txt") == "a tool\n");
        CHECK(r.downloaded_bytes == a.size);
        const std::vector<FakeHttpServer::Request> seen = server.requests();
        REQUIRE(seen.size() >= 2);
        CHECK(seen[0].range_start == -1);
        CHECK(seen.back().status == 206);
        CHECK(seen.back().range_start > 0);
    }
    SECTION("a server that ignores ranges restarts the file") {
        write_all(root / "downloads" / "toolchain.zip.part", body.substr(0, 50000));
        FakeHttpServer::Behaviour b;
        b.ignore_range = true;
        FakeHttpServer server("/toolchain.zip", body, b);
        const DownloadResult r = download_toolchain(options_for(a, *cmake, root, server.url()));
        CHECK(read_all(r.root / "bin" / "tool.txt") == "a tool\n");
        const std::vector<FakeHttpServer::Request> seen = server.requests();
        REQUIRE_FALSE(seen.empty());
        CHECK(seen.front().range_start == 50000);
        CHECK(seen.back().status == 200);
    }
    SECTION("an HTTP error ends the download") {
        FakeHttpServer::Behaviour b;
        b.status = 404;
        FakeHttpServer server("/toolchain.zip", body, b);
        CHECK(code_of([&] { (void)download_toolchain(options_for(a, *cmake, root, server.url())); }) ==
              ErrorCode::download_failed);
        CHECK(server.requests().size() == 3);
        CHECK_FALSE(installed_toolchain(root, options_for(a, *cmake, root, server.url()).pin).has_value());
    }
}

// The real pinned archive, from a local copy: DQ8_TEST_LLVM_MINGW_ZIP names it (the Windows-hosted llvm-mingw
// zip, 190 MB) and DQ8_TEST_TOOLCHAIN_ROOT where to unpack it. Hidden ([.]); run by hand on Windows or under
// Wine.
TEST_CASE("toolchain download: the pinned llvm-mingw archive unpacks as pinned",
          "[.][toolchain][download][real]") {
    const char* zip = std::getenv("DQ8_TEST_LLVM_MINGW_ZIP");
    const char* root = std::getenv("DQ8_TEST_TOOLCHAIN_ROOT");
    const std::optional<fs::path> cmake = test_cmake();
    if (zip == nullptr || root == nullptr || !cmake) {
        SKIP("set DQ8_TEST_LLVM_MINGW_ZIP and DQ8_TEST_TOOLCHAIN_ROOT (and have a cmake for this platform)");
    }
    DownloadOptions o;
    o.consent = true;
    o.bundled_root = root;
    o.cmake = *cmake;
    o.keep_archive = true;
    o.pin.url = file_url(zip);
    std::uint64_t unpacked = 0;
    o.on_progress = [&](const DownloadProgress& p) {
        if (p.phase == DownloadPhase::unpack) {
            unpacked = p.done;
        }
    };
    const DownloadResult r = download_toolchain(o);
    CHECK(r.root == fs::path(root) / pinned_llvm_mingw().version);
    CHECK(fs::is_regular_file(r.root / "bin" / "x86_64-w64-mingw32-clang++.exe"));
    CHECK(fs::is_regular_file(r.root / "x86_64-w64-mingw32" / "bin" / "libc++.dll"));
    if (!r.already_installed) {
        CHECK(unpacked == pinned_llvm_mingw().entries);
        std::uint64_t bytes = 0;
        for (const fs::directory_entry& e : fs::recursive_directory_iterator(r.root)) {
            if (e.is_regular_file()) {
                bytes += e.file_size();
            }
        }
        CHECK(bytes == pinned_llvm_mingw().unpacked_bytes + fs::file_size(r.root / ".dq8-toolchain"));
    }
}
