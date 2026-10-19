// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Runs against the user's own disc (DQ8_TEST_ISO). Checks only names, sizes and hashes that the region
// profile already records; nothing from the disc is written anywhere but a temporary directory.
#include "dq8/core/error.h"
#include "dq8/core/intake.h"
#include "dq8/core/iso9660.h"
#include "dq8/core/region_profile.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path test_iso() {
    const char* env = std::getenv("DQ8_TEST_ISO");
    if (env == nullptr || *env == '\0') {
        SKIP("DQ8_TEST_ISO is not set");
    }
    return env;
}

std::filesystem::path regions_dir() {
    return std::filesystem::path(DQ8_SOURCE_DIR) / "data" / "regions";
}

struct ScopedTempDir {
    std::filesystem::path path;
    ScopedTempDir() {
        path = std::filesystem::temp_directory_path() / ("dq8-local-" + std::to_string(std::rand()));
        std::filesystem::create_directories(path);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("the real disc opens and lists the expected files", "[local][iso]") {
    const auto iso = test_iso();
    const auto image = dq8::core::iso::Image::open(iso);
    CHECK(image.volume().system_identifier == "PLAYSTATION");
    CHECK(image.volume().logical_block_size == 2048);
    CHECK(image.find("SYSTEM.CNF") != nullptr);
    CHECK(image.find("SLUS_212.07") != nullptr);
    CHECK(image.find("BIN/TITLE.BIN") != nullptr);
    CHECK(image.find("MODULES/IOPRP300.IMG") != nullptr);
    CHECK(image.find("DMYDATA") != nullptr); // recorded as "DMYDATA.;1"
    CHECK(image.find("DATA.DAT")->size == 1804218368u);
    CHECK(image.volume().path_table_directories == 6);
    std::size_t files = 0;
    for (const auto& e : image.entries()) {
        if (!e.is_directory()) {
            ++files;
        }
    }
    CHECK(files == 68);
}

TEST_CASE("the real disc verifies as SLUS_21207", "[local][intake]") {
    const auto iso = test_iso();
    const auto profiles = dq8::core::load_region_profiles(regions_dir());
    const auto v = dq8::core::verify_image(iso, profiles);
    CHECK(v.profile.id == "SLUS_21207");
    CHECK(v.boot.size == 2960032u);
    CHECK(v.boot.sha1 == "f24bfd21c21f6454c05934224ab52cde837bc90b");
    CHECK(v.boot.crc32 == "a66a08b0");
    CHECK(v.elf.entry == 0x00100008u);
    CHECK(v.elf.load_segments().size() == 8);
    CHECK(v.system_cnf.vmode == "NTSC");
    for (const auto& w : v.warnings) {
        WARN(w);
    }
}

TEST_CASE("the real disc extracts to a temporary directory with the recorded sizes", "[local][intake]") {
    const auto iso = test_iso();
    const auto profiles = dq8::core::load_region_profiles(regions_dir());
    const auto v = dq8::core::verify_image(iso, profiles);
    ScopedTempDir tmp;
    dq8::core::ExtractOptions opts;
    opts.emit_profile = true;
    const auto report = dq8::core::extract(v, tmp.path / "baserom", opts);
    CHECK_FALSE(report.has_mismatch());
    REQUIRE(report.files.size() == 1 + 6 + 18);
    CHECK(report.count(dq8::core::FileStatus::extracted) == report.files.size());
    const auto base = tmp.path / "baserom" / "SLUS_21207";
    CHECK(std::filesystem::file_size(base / "SLUS_212.07") == 2960032u);
    CHECK(std::filesystem::file_size(base / "BIN" / "CASINO.BIN") == 129920u);
    CHECK(std::filesystem::file_size(base / "BIN" / "VIEWER.BIN") == 1152u);
    CHECK(std::filesystem::file_size(base / "BIN" / "SHOP.BIN") == 86016u);
    CHECK(std::filesystem::file_size(base / "BIN" / "MENU.BIN") == 109312u);
    CHECK(std::filesystem::file_size(base / "BIN" / "BATTLE.BIN") == 156032u);
    CHECK(std::filesystem::file_size(base / "BIN" / "TITLE.BIN") == 20864u);
    CHECK(std::filesystem::file_size(base / "MODULES" / "IOPRP300.IMG") == 275345u);
    CHECK(std::filesystem::is_regular_file(base / "manifest.toml"));
    CHECK(std::filesystem::is_regular_file(tmp.path / "baserom" / ".gitignore"));
    CHECK_FALSE(std::filesystem::exists(base / "DATA.DAT"));
    CHECK_FALSE(std::filesystem::exists(base / "MOVIE"));
    CHECK_FALSE(report.profile_fragment.empty());

    const auto again = dq8::core::extract(v, tmp.path / "baserom", opts);
    CHECK(again.count(dq8::core::FileStatus::up_to_date) == again.files.size());
}
