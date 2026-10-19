// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/error.h"
#include "dq8/core/hash.h"
#include "dq8/core/intake.h"
#include "support/iso_builder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <fstream>
#include <string>

using Catch::Matchers::ContainsSubstring;
using dq8::core::Error;
using dq8::core::ErrorCode;
using dq8::core::FileStatus;
using dq8::test::seeded_bytes;

namespace {

constexpr std::uint32_t entry_point = 0x00100008;
constexpr std::uint32_t load_address = 0x00100000;
constexpr std::uint32_t main_memsz = 0x4000;
constexpr std::uint32_t window_base = 0x00200000;
const std::vector<std::uint32_t> overlay_memsz{0x300, 0x80};

std::vector<std::uint8_t> text_bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

// A synthetic disc whose boot file is a fake ELF of seeded random bytes, plus two "overlays" and a module.
struct Fixture {
    dq8::test::TempDir tmp;
    std::filesystem::path iso;
    std::filesystem::path regions;
    std::vector<std::uint8_t> boot;
    std::vector<std::uint8_t> ov_a = seeded_bytes(21, 3000);
    std::vector<std::uint8_t> ov_b = seeded_bytes(22, 700);
    std::vector<std::uint8_t> mod = seeded_bytes(23, 4097);

    explicit Fixture(std::uint32_t seed = 77, bool with_hashes = true) {
        boot = dq8::test::fake_boot_elf(entry_point, load_address, main_memsz, window_base, overlay_memsz,
                                        9000, seed);
        dq8::test::IsoBuilder b;
        b.set_system_identifier("PLAYSTATION");
        b.add_file("/SYSTEM.CNF",
                   text_bytes("BOOT2 = cdrom0:\\BOOT.ELF;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n"));
        b.add_file("/BOOT.ELF", boot);
        b.add_file("/BIN/A.BIN", ov_a);
        b.add_file("/BIN/B.BIN", ov_b);
        b.add_file("/MODULES/X.IRX", mod);
        b.add_file("/DATA.DAT", seeded_bytes(24, 5000));
        b.add_file("/MOVIE/M.MVI", seeded_bytes(25, 10));
        iso = tmp.path() / "synthetic.iso";
        b.write(iso);
        regions = tmp.path() / "regions";
        std::filesystem::create_directories(regions);
        write_profile(with_hashes);
    }

    std::string hashes_of(const std::vector<std::uint8_t>& data, bool with) const {
        if (!with) {
            return "size = 0\nsha1 = \"\"\ncrc32 = \"\"\n";
        }
        return "size = " + std::to_string(data.size()) + "\nsha1 = \"" +
               dq8::core::to_hex(dq8::core::Sha1::of(data)) + "\"\ncrc32 = \"" +
               dq8::core::to_hex(dq8::core::Crc32::of(data)) + "\"\n";
    }

    void write_profile(bool with_hashes) const {
        std::string text =
            "schema_version = 1\n[profile]\nid = \"TEST_00001\"\nserial = \"TEST-00001\"\n"
            "title = \"Synthetic\"\nregion = \"NTSC-U\"\nvideo_mode = \"NTSC\"\n"
            "[image]\nsize = " +
            std::to_string(std::filesystem::file_size(iso)) +
            "\nsector_size = 2048\n"
            "[boot]\npath = \"BOOT.ELF\"\n" +
            hashes_of(boot, true) +
            "entry = 0x00100008\nload_address = 0x00100000\nmain_memsz = 0x4000\n"
            "[overlay_window]\nbase = 0x00200000\nend = 0x00210000\ncontainer = \"MWo3\"\nheader_size = 64\n"
            "[[overlays]]\nname = \"A\"\nid = 1\npath = \"BIN/A.BIN\"\nsegment_memsz = 0x300\n" +
            hashes_of(ov_a, with_hashes) +
            "[[overlays]]\nname = \"B\"\nid = 2\npath = \"BIN/B.BIN\"\nsegment_memsz = 0x80\n" +
            hashes_of(ov_b, with_hashes) + "[[files]]\npath = \"MODULES/X.IRX\"\nrole = \"iop_module\"\n" +
            hashes_of(mod, with_hashes) + "[not_extracted]\npaths = [\"DATA.DAT\", \"MOVIE/\"]\n";
        std::ofstream(regions / "TEST_00001.toml", std::ios::binary) << text;
    }
};

} // namespace

TEST_CASE("SYSTEM.CNF parsing", "[core][intake]") {
    const auto cnf =
        dq8::core::parse_system_cnf("BOOT2 = cdrom0:\\SLUS_212.07;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n");
    CHECK(cnf.boot2 == "cdrom0:\\SLUS_212.07;1");
    CHECK(cnf.boot_path == "SLUS_212.07");
    CHECK(cnf.ver == "1.00");
    CHECK(cnf.vmode == "NTSC");
    CHECK(cnf.entries.size() == 3);
    CHECK(dq8::core::parse_system_cnf("boot2=cdrom0:/X.ELF").boot_path == "X.ELF");
    CHECK_THROWS_AS(dq8::core::parse_system_cnf("VER = 1.00\n"), Error);
    CHECK_THROWS_AS(dq8::core::parse_system_cnf("BOOT2 = mc0:/X.ELF\n"), Error);
    CHECK_THROWS_AS(dq8::core::parse_system_cnf("garbage\n"), Error);
}

TEST_CASE("verify identifies a synthetic disc and cross-checks the ELF", "[core][intake]") {
    const Fixture fx;
    const auto profiles = dq8::core::load_region_profiles(fx.regions);
    const auto v = dq8::core::verify_image(fx.iso, profiles);
    CHECK(v.profile.id == "TEST_00001");
    CHECK(v.boot_path == "/BOOT.ELF");
    CHECK(v.boot.size == fx.boot.size());
    CHECK(v.boot.sha1 == dq8::core::to_hex(dq8::core::Sha1::of(fx.boot)));
    CHECK(v.elf.entry == entry_point);
    CHECK(v.elf.load_segments().size() == 3);
    CHECK(v.system_cnf.vmode == "NTSC");
    CHECK(v.warnings.empty());
}

TEST_CASE("verify reports an unknown boot file with its hash and size", "[core][intake]") {
    const Fixture fx(77);
    const Fixture other(78); // a different seed: a boot file no profile knows
    const auto profiles = dq8::core::load_region_profiles(fx.regions);
    try {
        (void)dq8::core::verify_image(other.iso, profiles);
        FAIL("expected unknown_image");
    } catch (const Error& e) {
        CHECK(e.code() == ErrorCode::unknown_image);
        CHECK(dq8::core::is_mismatch(e.code()));
        CHECK_THAT(e.what(), ContainsSubstring(dq8::core::to_hex(dq8::core::Sha1::of(other.boot))));
        CHECK_THAT(e.what(), ContainsSubstring(std::to_string(other.boot.size())));
        CHECK_THAT(e.what(), ContainsSubstring("TEST_00001"));
    }
}

TEST_CASE("verify refuses discs without SYSTEM.CNF or without the boot file", "[core][intake]") {
    dq8::test::TempDir tmp;
    const Fixture fx;
    const auto profiles = dq8::core::load_region_profiles(fx.regions);

    dq8::test::IsoBuilder no_cnf;
    no_cnf.add_file("/README.TXT", text_bytes("hello"));
    const auto iso1 = tmp.path() / "nocnf.iso";
    no_cnf.write(iso1);
    try {
        (void)dq8::core::verify_image(iso1, profiles);
        FAIL("expected system_cnf");
    } catch (const Error& e) {
        CHECK(e.code() == ErrorCode::system_cnf);
    }

    dq8::test::IsoBuilder no_boot;
    no_boot.add_file("/SYSTEM.CNF", text_bytes("BOOT2 = cdrom0:\\GONE.ELF;1\n"));
    const auto iso2 = tmp.path() / "noboot.iso";
    no_boot.write(iso2);
    try {
        (void)dq8::core::verify_image(iso2, profiles);
        FAIL("expected boot_file_missing");
    } catch (const Error& e) {
        CHECK(e.code() == ErrorCode::boot_file_missing);
    }
}

TEST_CASE("verify detects a profile whose ELF facts disagree with the disc", "[core][intake]") {
    const Fixture fx;
    std::string text;
    {
        std::ifstream in(fx.regions / "TEST_00001.toml", std::ios::binary);
        text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    auto expect_mismatch = [&](const std::string& from, const std::string& to, const std::string& fragment) {
        std::string edited = text;
        const auto pos = edited.find(from);
        REQUIRE(pos != std::string::npos);
        edited.replace(pos, from.size(), to);
        std::ofstream(fx.regions / "TEST_00001.toml", std::ios::binary) << edited;
        const auto profiles = dq8::core::load_region_profiles(fx.regions);
        try {
            (void)dq8::core::verify_image(fx.iso, profiles);
            FAIL("expected image_mismatch for " << fragment);
        } catch (const Error& e) {
            CHECK(e.code() == ErrorCode::image_mismatch);
            CHECK_THAT(e.what(), ContainsSubstring(fragment));
        }
    };
    expect_mismatch("entry = 0x00100008", "entry = 0x00100010", "entry point");
    expect_mismatch("main_memsz = 0x4000", "main_memsz = 0x4080", "main segment memsz");
    expect_mismatch("segment_memsz = 0x80", "segment_memsz = 0x90", "overlay segment 1");
    expect_mismatch("path = \"MODULES/X.IRX\"", "path = \"MODULES/Y.IRX\"", "does not contain it");
    expect_mismatch("size = 4097", "size = 4096", "4097 bytes on the disc");
}

TEST_CASE("extract writes, hashes, records and is idempotent", "[core][intake]") {
    const Fixture fx;
    const auto profiles = dq8::core::load_region_profiles(fx.regions);
    const auto v = dq8::core::verify_image(fx.iso, profiles);
    const auto out = fx.tmp.path() / "baserom";

    dq8::core::ExtractOptions opts;
    std::size_t progress_calls = 0;
    opts.progress = [&](std::string_view, std::uint64_t, std::uint64_t) { ++progress_calls; };
    const auto first = dq8::core::extract(v, out, opts);
    CHECK(first.root == out);
    CHECK(first.profile_dir == out / "TEST_00001");
    REQUIRE(first.files.size() == 4);
    CHECK(first.count(FileStatus::extracted) == 4);
    CHECK_FALSE(first.has_mismatch());
    CHECK(progress_calls >= 4);
    CHECK(first.warnings.empty());

    // The files, their contents, the .gitignore and the manifest.
    CHECK(std::filesystem::file_size(out / "TEST_00001" / "BOOT.ELF") == fx.boot.size());
    CHECK(std::filesystem::file_size(out / "TEST_00001" / "BIN" / "A.BIN") == fx.ov_a.size());
    CHECK(std::filesystem::file_size(out / "TEST_00001" / "MODULES" / "X.IRX") == fx.mod.size());
    CHECK(dq8::core::digest_file(out / "TEST_00001" / "MODULES" / "X.IRX").sha1 ==
          dq8::core::to_hex(dq8::core::Sha1::of(fx.mod)));
    CHECK_FALSE(std::filesystem::exists(out / "TEST_00001" / "DATA.DAT"));
    CHECK_FALSE(std::filesystem::exists(out / "TEST_00001" / "MOVIE"));
    {
        std::ifstream gi(out / ".gitignore");
        std::string content((std::istreambuf_iterator<char>(gi)), std::istreambuf_iterator<char>());
        CHECK(content == "*\n");
    }
    CHECK(std::filesystem::is_regular_file(first.manifest));

    // Second run: everything is up to date, nothing rewritten.
    const auto before = std::filesystem::last_write_time(out / "TEST_00001" / "BOOT.ELF");
    const auto second = dq8::core::extract(v, out, opts);
    CHECK(second.count(FileStatus::up_to_date) == 4);
    CHECK(std::filesystem::last_write_time(out / "TEST_00001" / "BOOT.ELF") == before);

    // A damaged file is re-extracted even without --force.
    {
        std::ofstream(out / "TEST_00001" / "BIN" / "B.BIN", std::ios::binary | std::ios::trunc) << "damaged";
    }
    const auto third = dq8::core::extract(v, out, opts);
    CHECK(third.count(FileStatus::up_to_date) == 3);
    CHECK(third.count(FileStatus::extracted) == 1);
    CHECK(std::filesystem::file_size(out / "TEST_00001" / "BIN" / "B.BIN") == fx.ov_b.size());

    // --force rewrites everything.
    opts.force = true;
    const auto fourth = dq8::core::extract(v, out, opts);
    CHECK(fourth.count(FileStatus::extracted) == 4);
}

TEST_CASE("extract reports hash mismatches against the profile and --emit-profile fills them",
          "[core][intake]") {
    // Profile without overlay/module hashes: nothing to compare, and the fragment carries the real ones.
    const Fixture fx(77, false);
    const auto profiles = dq8::core::load_region_profiles(fx.regions);
    const auto v = dq8::core::verify_image(fx.iso, profiles);
    const auto out = fx.tmp.path() / "baserom";
    dq8::core::ExtractOptions opts;
    opts.emit_profile = true;
    const auto report = dq8::core::extract(v, out, opts);
    CHECK_FALSE(report.has_mismatch());
    CHECK_THAT(report.profile_fragment, ContainsSubstring("[[overlays]]"));
    CHECK_THAT(report.profile_fragment, ContainsSubstring("path = \"BIN/A.BIN\""));
    CHECK_THAT(report.profile_fragment,
               ContainsSubstring("sha1 = \"" + dq8::core::to_hex(dq8::core::Sha1::of(fx.ov_a)) + "\""));
    CHECK_THAT(report.profile_fragment,
               ContainsSubstring("crc32 = \"" + dq8::core::to_hex(dq8::core::Crc32::of(fx.mod)) + "\""));
    CHECK_THAT(report.profile_fragment, ContainsSubstring("segment_memsz = 0x300"));
    CHECK_THAT(report.profile_fragment, ContainsSubstring("role = \"iop_module\""));

    // Now a profile that records a wrong hash for one overlay: extraction still completes, but reports it.
    std::string text;
    {
        std::ifstream in(fx.regions / "TEST_00001.toml", std::ios::binary);
        text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    const std::string wrong = "0123456789abcdef0123456789abcdef01234567";
    const auto pos = text.find("segment_memsz = 0x80\nsize = 0\nsha1 = \"\"");
    REQUIRE(pos != std::string::npos);
    text.replace(pos, std::string("segment_memsz = 0x80\nsize = 0\nsha1 = \"\"").size(),
                 "segment_memsz = 0x80\nsize = 0\nsha1 = \"" + wrong + "\"");
    std::ofstream(fx.regions / "TEST_00001.toml", std::ios::binary) << text;
    const auto profiles2 = dq8::core::load_region_profiles(fx.regions);
    const auto v2 = dq8::core::verify_image(fx.iso, profiles2);
    dq8::core::ExtractOptions force;
    force.force = true;
    const auto report2 = dq8::core::extract(v2, out, force);
    CHECK(report2.has_mismatch());
    CHECK(report2.count(FileStatus::hash_mismatch) == 1);
    for (const auto& f : report2.files) {
        if (f.disc_path == "BIN/B.BIN") {
            CHECK(f.status == FileStatus::hash_mismatch);
            CHECK(f.expected_sha1 == wrong);
            CHECK(f.digest.sha1 == dq8::core::to_hex(dq8::core::Sha1::of(fx.ov_b)));
        }
    }
    // With verification off the same run is clean.
    force.verify_hashes = false;
    CHECK_FALSE(dq8::core::extract(v2, out, force).has_mismatch());
}
