// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "core/platform/paths.h"
#include "runtime/io/memcard/mc_path.h"
#include "runtime/io/memcard/memory_card.h"
#include "runtime/io/memcard/memory_card_store.h"
#include "runtime/machine/rtc.h"
#include "support/iso_builder.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rt = dq8::rt;
namespace mc = dq8::rt::mc;
namespace fs = std::filesystem;
using dq8::test::TempDir;

namespace {

const rt::RtcDateTime t0{2005, 11, 15, 12, 0, 0};
const rt::RtcDateTime t1{2005, 11, 15, 12, 0, 30};
const rt::RtcDateTime t2{2005, 11, 16, 9, 15, 0};

std::vector<std::uint8_t> bytes(std::size_t n, std::uint8_t seed) {
    std::vector<std::uint8_t> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::uint8_t>(seed + i * 7);
    }
    return out;
}

std::vector<std::uint8_t> file_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A save the way the game makes one: a directory, files created and written in chunks, flushed and closed.
void make_save(mc::MemoryCard& card, std::string_view dir, const std::vector<std::uint8_t>& data,
               const rt::RtcDateTime& now) {
    REQUIRE(card.open(dir, mc::open_mkdir, now) == mc::result_ok);
    const std::string file = std::string(dir) + "/data.bin";
    const std::int32_t fd = card.open(file, mc::open_create | mc::open_write, now);
    REQUIRE(fd >= 0);
    for (std::size_t at = 0; at < data.size(); at += 0xC00) {
        const std::size_t n = std::min<std::size_t>(0xC00, data.size() - at);
        REQUIRE(card.write(fd, std::span(data).subspan(at, n)) == static_cast<std::int32_t>(n));
    }
    REQUIRE(card.flush(fd, now) == mc::result_ok);
    REQUIRE(card.close(fd, now) == mc::result_ok);
}

std::vector<std::uint8_t> read_all(mc::MemoryCard& card, std::string_view path) {
    const std::int32_t fd = card.open(path, mc::open_read, t1);
    REQUIRE(fd >= 0);
    std::vector<std::uint8_t> out;
    for (;;) {
        std::vector<std::uint8_t> chunk;
        const std::int32_t n = card.read(fd, 0x1000, chunk);
        REQUIRE(n >= 0);
        if (n == 0) {
            break;
        }
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    REQUIRE(card.close(fd, t1) == mc::result_ok);
    return out;
}

std::vector<std::string> names(const std::vector<mc::DirEntry>& entries) {
    std::vector<std::string> out;
    for (const auto& e : entries) {
        out.push_back(e.name);
    }
    return out;
}

} // namespace

TEST_CASE("rtc: dates, seconds and virtual time", "[memcard][rtc]") {
    CHECK(rt::format_rtc(rt::VirtualRtc::default_epoch()) == "2005-11-15T12:00:00");
    CHECK(rt::parse_rtc("2004-02-29T23:59:59") == rt::RtcDateTime{2004, 2, 29, 23, 59, 59});
    CHECK_FALSE(rt::parse_rtc("2005-02-29T00:00:00"));
    CHECK_FALSE(rt::parse_rtc("1999-12-31T23:59:59"));
    CHECK_FALSE(rt::parse_rtc("2005-11-15 12:00:00"));
    CHECK(rt::rtc_from_seconds(rt::rtc_seconds(t2)) == t2);
    CHECK(rt::rtc_seconds(rt::RtcDateTime{2000, 1, 1, 0, 0, 0}) == 0);
    CHECK(rt::rtc_seconds(rt::RtcDateTime{2000, 3, 1, 0, 0, 0}) == 60 * 86400);

    const rt::VirtualRtc rtc(rt::RtcDateTime{2005, 12, 31, 23, 59, 58});
    CHECK(rtc.at(0) == rt::RtcDateTime{2005, 12, 31, 23, 59, 58});
    CHECK(rtc.at(rt::ee_clock_hz - 1) == rt::RtcDateTime{2005, 12, 31, 23, 59, 58}); // whole seconds
    CHECK(rtc.at(2 * rt::ee_clock_hz) == rt::RtcDateTime{2006, 1, 1, 0, 0, 0});
}

TEST_CASE("memcard: paths and wildcards", "[memcard]") {
    CHECK(mc::join_path(mc::resolve_path("/A/B/../C/./D", {})) == "/A/C/D");
    CHECK(mc::join_path(mc::resolve_path("mc0:/A", {})) == "/A");
    CHECK(mc::join_path(mc::resolve_path("X", {"A"})) == "/A/X");
    CHECK(mc::join_path(mc::resolve_path("..\\..", {"A"})) == "/");
    CHECK(mc::wildcard_match("BASLUS-21207dq8???", "BASLUS-21207dq8_1"));
    CHECK(mc::wildcard_match("BASLUS-21207dq8???", "BASLUS-21207dq8_12"));
    CHECK_FALSE(mc::wildcard_match("BASLUS-21207dq8???", "BASLUS-21207dq8_1234"));
    CHECK_FALSE(mc::wildcard_match("a?c", "ac"));
    CHECK(mc::wildcard_match("*", "."));
    CHECK(mc::wildcard_match("a*c", "abbbc"));
    CHECK_FALSE(mc::wildcard_match("a*c", "abcd"));
}

TEST_CASE("memcard: GetInfo reports the insertion once, then 0, with the format flag", "[memcard]") {
    mc::MemoryCard blank(false, t0);
    auto info = blank.get_info();
    CHECK(info.result == mc::result_no_format);
    CHECK(info.type == mc::card_type_ps2);
    CHECK(info.format == 0);
    CHECK(info.free_clusters == 0);
    info = blank.get_info();
    CHECK(info.result == mc::result_ok);
    CHECK(info.format == 0);

    // Requests on an unformatted card say so; formatting does not count as an insertion.
    CHECK(blank.open("/X", mc::open_mkdir, t0) == mc::result_no_format);
    std::vector<mc::DirEntry> list;
    CHECK(blank.get_dir("/*", 8, list) == mc::result_no_format);
    CHECK(blank.format(t1) == mc::result_ok);
    info = blank.get_info();
    CHECK(info.result == mc::result_ok);
    CHECK(info.format == 1);
    CHECK(info.free_clusters == 8000);

    mc::MemoryCard formatted(true, t0);
    CHECK(formatted.get_info().result == mc::result_new_card);
    CHECK(formatted.get_info().result == mc::result_ok);
    CHECK(formatted.unformat() == mc::result_ok);
    info = formatted.get_info();
    CHECK(info.result == mc::result_ok);
    CHECK(info.format == 0);
    CHECK(info.free_clusters == 0);
}

TEST_CASE("memcard: format, write, read, getdir and delete", "[memcard]") {
    mc::MemoryCard card(false, t0);
    REQUIRE(card.format(t0) == mc::result_ok);
    const auto data = bytes(0x5CE0, 3);
    make_save(card, "/SAVE_1", data, t1);
    CHECK(read_all(card, "/SAVE_1/data.bin") == data);

    // The listing: "." and ".." first, then the entries in card order, with their sizes and times.
    std::vector<mc::DirEntry> list;
    REQUIRE(card.get_dir("/SAVE_1/*", 8, list) == 3);
    CHECK(names(list) == std::vector<std::string>{".", "..", "data.bin"});
    CHECK(list[0].size == 3);
    CHECK(list[2].size == 0x5CE0);
    CHECK(list[2].created == t1);
    CHECK(list[2].modified == t1);
    CHECK(list[2].attr == mc::file_attr);
    CHECK(list[0].attr == mc::dir_attr);
    REQUIRE(card.get_dir("/SAVE_?", 8, list) == 1);
    CHECK(list[0].name == "SAVE_1");
    CHECK(card.get_dir("/SAVE_1/data.bin", 1, list) == 1); // a plain name: that entry only
    CHECK(card.get_dir("/NONE/*", 8, list) == mc::result_no_entry);
    CHECK(card.get_dir("/SAVE_1/*", 2, list) == 2); // maxent
    CHECK(card.get_dir("/SAVE_1/*", 0, list) == 0);

    // 0x5CE0 bytes take 24 clusters; the directory (3 entries with . and ..) 2, and so does the root now.
    CHECK(card.free_clusters() == 8001 - 2 - 2 - 24);

    // Deletion: a directory that is not empty refuses, an open file refuses, then both go.
    CHECK(card.remove("/SAVE_1", t2) == mc::result_not_empty);
    const std::int32_t fd = card.open("/SAVE_1/data.bin", mc::open_read, t2);
    CHECK(card.remove("/SAVE_1/data.bin", t2) == mc::result_denied);
    CHECK(card.close(fd, t2) == mc::result_ok);
    CHECK(card.remove("/SAVE_1/data.bin", t2) == mc::result_ok);
    CHECK(card.remove("/SAVE_1", t2) == mc::result_ok);
    CHECK(card.remove("/SAVE_1", t2) == mc::result_no_entry);
    CHECK(card.get_dir("/*", 8, list) == 2);
}

TEST_CASE("memcard: the results the game distinguishes", "[memcard]") {
    mc::MemoryCard card(true, t0);
    CHECK(card.open("/D", mc::open_mkdir, t0) == mc::result_ok);
    CHECK(card.open("/D", mc::open_mkdir, t0) == mc::result_no_entry); // exists: tolerated by the game
    CHECK(card.open("/D/missing", mc::open_read, t0) == mc::result_no_entry);
    CHECK(card.open("/missing/x", mc::open_create | mc::open_write, t0) == mc::result_no_entry);
    CHECK(card.open("/D", mc::open_read, t0) == mc::result_denied);
    CHECK(card.close(2, t0) == mc::result_no_entry);

    // Three handles, the lowest free first.
    const std::int32_t a = card.open("/D/a", mc::open_create | mc::open_access, t0);
    const std::int32_t b = card.open("/D/b", mc::open_create | mc::open_access, t0);
    const std::int32_t c = card.open("/D/c", mc::open_create | mc::open_access, t0);
    CHECK((a == 0 && b == 1 && c == 2));
    CHECK(card.open("/D/d", mc::open_create | mc::open_access, t0) == mc::result_handles);
    CHECK(card.close(b, t0) == mc::result_ok);
    CHECK(card.open("/D/d", mc::open_create | mc::open_access, t0) == 1);

    // Access modes and seeks.
    const std::int32_t ro = card.open("/D/a", mc::open_read, t0);
    CHECK(ro == mc::result_handles);
    CHECK(card.close(c, t0) == mc::result_ok);
    const std::int32_t r = card.open("/D/a", mc::open_read, t0);
    const std::vector<std::uint8_t> one{0x42};
    CHECK(card.write(r, one) == mc::result_denied);
    CHECK(card.write(a, bytes(10, 1)) == 10);
    CHECK(card.seek(a, 4, 0) == 4);
    CHECK(card.write(a, one) == 1);
    CHECK(card.seek(a, -1, 1) == 4);
    CHECK(card.seek(a, 0, 2) == 10);
    CHECK(card.seek(a, -11, 2) == mc::result_denied);
    std::vector<std::uint8_t> got;
    CHECK(card.read(r, 16, got) == 10);
    CHECK(got[4] == 0x42);
}

TEST_CASE("memcard: a freed slot is reused, so entry order is deterministic", "[memcard]") {
    mc::MemoryCard card(true, t0);
    for (const char* d : {"/A", "/B", "/C"}) {
        REQUIRE(card.open(d, mc::open_mkdir, t0) == mc::result_ok);
    }
    REQUIRE(card.remove("/B", t1) == mc::result_ok);
    REQUIRE(card.open("/D", mc::open_mkdir, t1) == mc::result_ok);
    REQUIRE(card.open("/E", mc::open_mkdir, t1) == mc::result_ok);
    std::vector<mc::DirEntry> list;
    REQUIRE(card.get_dir("/*", 16, list) == 6);
    CHECK(names(list) == std::vector<std::string>{".", "..", "A", "D", "C", "E"});
    CHECK(card.root().modified == t1);
}

TEST_CASE("memcard: rename and file information", "[memcard]") {
    mc::MemoryCard card(true, t0);
    make_save(card, "/S", bytes(964, 9), t0);
    mc::DirEntry info;
    info.name = "icon.err";
    CHECK(card.set_info("/S/data.bin", info, mc::info_rename) == mc::result_ok);
    std::vector<mc::DirEntry> list;
    REQUIRE(card.get_dir("/S/*", 8, list) == 3);
    CHECK(list[2].name == "icon.err");
    CHECK(list[2].modified == t0); // a rename keeps the times
    REQUIRE(card.open("/S/other", mc::open_create | mc::open_write, t0) >= 0);
    info.name = "other";
    CHECK(card.set_info("/S/icon.err", info, mc::info_rename) == mc::result_denied);
    info.name = "bad/name";
    CHECK(card.set_info("/S/icon.err", info, mc::info_rename) == mc::result_no_entry);

    info.created = t2;
    info.modified = t2;
    info.attr = mc::attr_hidden;
    CHECK(card.set_info("/S/icon.err", info, mc::info_created | mc::info_modified | mc::info_attr) ==
          mc::result_ok);
    REQUIRE(card.get_dir("/S/icon.err", 1, list) == 1);
    CHECK(list[0].created == t2);
    CHECK(list[0].modified == t2);
    CHECK((list[0].attr & mc::attr_hidden) != 0);
    CHECK((list[0].attr & mc::attr_file) != 0); // only the settable bits change
}

TEST_CASE("memcard: a full card writes what fits and then reports full", "[memcard]") {
    mc::MemoryCard card(true, t0);
    const std::int32_t fd = card.open("/big", mc::open_create | mc::open_write, t0);
    const std::uint32_t free = card.free_clusters();
    const auto huge = bytes(std::size_t{free + 1} * mc::cluster_bytes, 5);
    CHECK(card.write(fd, huge) == static_cast<std::int32_t>(free * mc::cluster_bytes));
    CHECK(card.free_clusters() == 0);
    CHECK(card.write(fd, bytes(1, 1)) == mc::result_full);
    CHECK(card.open("/more", mc::open_mkdir, t0) == mc::result_full);
}

TEST_CASE("memcard: the digest follows content, order and times", "[memcard]") {
    const auto build = [](const rt::RtcDateTime& when) {
        auto card = std::make_unique<mc::MemoryCard>(false, t0);
        card->format(t0);
        make_save(*card, "/S", bytes(3000, 1), when);
        return card;
    };
    CHECK(build(t1)->sha1() == build(t1)->sha1());
    CHECK(build(t1)->sha1() != build(t2)->sha1());
    CHECK(mc::MemoryCard(false, t0).sha1() == mc::MemoryCard(false, t2).sha1()); // unformatted: no content
    CHECK(mc::MemoryCard(true, t0).sha1() != mc::MemoryCard(false, t0).sha1());
}

TEST_CASE("memcard: GetDir records are 0x40 bytes in the card's layout", "[memcard]") {
    mc::DirEntry e{t1, t2, 0x3C4, mc::file_attr, "icon.sys"};
    const auto raw = mc::encode_dir_entry(e);
    CHECK(raw[0x01] == 30); // created seconds
    CHECK(raw[0x04] == 15);
    CHECK(raw[0x05] == 11);
    CHECK((raw[0x06] | raw[0x07] << 8) == 2005);
    CHECK(raw[0x0B] == 9); // modified hour
    CHECK((raw[0x10] | raw[0x11] << 8) == 0x3C4);
    CHECK((raw[0x14] | raw[0x15] << 8) == mc::file_attr);
    CHECK(std::string(reinterpret_cast<const char*>(raw.data() + 0x20)) == "icon.sys");
    const mc::DirEntry back = mc::decode_dir_entry(raw);
    CHECK(back.name == e.name);
    CHECK(back.created == t1);
    CHECK(back.modified == t2);
    CHECK(back.size == e.size);
}

TEST_CASE("memcard folder: host names", "[memcard][folder]") {
    CHECK(rt::host_file_name("BASLUS-21207dq8_1") == "BASLUS-21207dq8_1");
    CHECK(rt::host_file_name("icon.sys") == "icon.sys");
    CHECK(rt::host_file_name(".hidden") == "%2Ehidden");
    CHECK(rt::host_file_name("a:b?") == "a%3Ab%3F");
    CHECK(rt::host_file_name("CON") == "%43ON");
    CHECK(rt::host_file_name("50%") == "50%25");
    CHECK(rt::card_name_from_host("a%3Ab%3F") == "a:b?");
    CHECK(rt::card_name_from_host("bad%4") == "");
}

TEST_CASE("memcard folder: a card is written behind and loads back the same", "[memcard][folder]") {
    TempDir dir;
    const fs::path folder = dir.path() / "SLUS_21207" / "card1";
    const auto data = bytes(0x5CE0, 3);
    std::string digest;
    {
        auto store = rt::open_folder_card_store(folder, false, t0);
        mc::MemoryCard& card = store->card();
        CHECK(card.get_info().result == mc::result_no_format);
        store->finish();
        CHECK_FALSE(fs::exists(folder)); // nothing is written before the guest changes something
        REQUIRE(card.format(t0) == mc::result_ok);
        make_save(card, "/SAVE_1", data, t1);
        REQUIRE(card.open("/SAVE_2", mc::open_mkdir, t1) == mc::result_ok);
        REQUIRE(card.open("/SAVE_3", mc::open_mkdir, t1) == mc::result_ok);
        REQUIRE(card.remove("/SAVE_2", t2) == mc::result_ok);
        mc::DirEntry info;
        info.name = "renamed.bin";
        REQUIRE(card.set_info("/SAVE_1/data.bin", info, mc::info_rename) == mc::result_ok);
        digest = card.sha1();
        store->finish();
        CHECK(store->write_errors() == 0);
    }
    CHECK(fs::exists(folder / "card.toml"));
    CHECK(fs::exists(folder / ".dir.toml"));
    CHECK(file_bytes(folder / "SAVE_1" / "renamed.bin") == data);
    CHECK_FALSE(fs::exists(folder / "SAVE_2"));

    // Host modification times play no part.
    fs::last_write_time(folder / "SAVE_1" / "renamed.bin",
                        fs::file_time_type::clock::now() - std::chrono::hours(48));

    auto again = rt::open_folder_card_store(folder, false, t2);
    mc::MemoryCard& card = again->card();
    CHECK(card.sha1() == digest);
    CHECK(card.get_info().result == mc::result_new_card);
    CHECK(read_all(card, "/SAVE_1/renamed.bin") == data);
    std::vector<mc::DirEntry> list;
    REQUIRE(card.get_dir("/*", 8, list) == 4);
    CHECK(names(list) == std::vector<std::string>{".", "..", "SAVE_1", "SAVE_3"});
    // The freed slot of SAVE_2 survives the reload: the next directory takes it.
    REQUIRE(card.open("/SAVE_4", mc::open_mkdir, t2) == mc::result_ok);
    REQUIRE(card.get_dir("/*", 8, list) == 5);
    CHECK(names(list) == std::vector<std::string>{".", "..", "SAVE_1", "SAVE_4", "SAVE_3"});
}

TEST_CASE("memcard folder: replaced and deleted files go to rolling backups", "[memcard][folder]") {
    TempDir dir;
    const fs::path folder = dir.path() / "card1";
    auto store = rt::open_folder_card_store(folder, true, t0);
    mc::MemoryCard& card = store->card();
    for (std::uint8_t generation = 1; generation <= 5; ++generation) {
        const std::int32_t fd = card.open("/f", mc::open_create | mc::open_write, t0);
        REQUIRE(fd >= 0);
        REQUIRE(card.write(fd, bytes(100, generation)) == 100);
        REQUIRE(card.close(fd, t0) == mc::result_ok);
        store->finish();
    }
    CHECK(file_bytes(folder / "f") == bytes(100, 5));
    CHECK(file_bytes(folder / ".backup" / "f.1") == bytes(100, 4));
    CHECK(file_bytes(folder / ".backup" / "f.2") == bytes(100, 3));
    CHECK(file_bytes(folder / ".backup" / "f.3") == bytes(100, 2));
    CHECK_FALSE(fs::exists(folder / ".backup" / "f.4"));

    REQUIRE(card.remove("/f", t1) == mc::result_ok);
    store->finish();
    CHECK_FALSE(fs::exists(folder / "f"));
    CHECK(file_bytes(folder / ".backup" / "f.1") == bytes(100, 5));
}

TEST_CASE("memcard folder: an interrupted write keeps the old content", "[memcard][folder]") {
    TempDir dir;
    const fs::path folder = dir.path() / "card1";
    bool crash = false;
    rt::FolderStoreOptions options;
    options.warn = [](const std::string&) {};
    options.fault = [&crash](std::string_view point) {
        if (crash && point == "temp-written") {
            throw std::runtime_error("simulated power cut");
        }
    };
    {
        auto store = rt::open_folder_card_store(folder, true, t0, options);
        mc::MemoryCard& card = store->card();
        make_save(card, "/S", bytes(2000, 1), t0);
        store->finish();
        crash = true;
        const std::int32_t fd = card.open("/S/data.bin", mc::open_write, t1);
        REQUIRE(card.write(fd, bytes(2000, 2)) == 2000);
        REQUIRE(card.close(fd, t1) == mc::result_ok); // the guest saw success: disk errors never reach it
        store->finish();
        CHECK(store->write_errors() == 1);
    }
    CHECK(file_bytes(folder / "S" / "data.bin") == bytes(2000, 1));
    auto again = rt::open_folder_card_store(folder, true, t1, options);
    CHECK(read_all(again->card(), "/S/data.bin") == bytes(2000, 1));
    CHECK_FALSE(fs::exists(folder / ".tmp")); // the abandoned temporary file is cleared at load
}

TEST_CASE("memcard folder: files copied in by hand are loaded in name order", "[memcard][folder]") {
    TempDir dir;
    const fs::path folder = dir.path() / "card1";
    {
        auto store = rt::open_folder_card_store(folder, true, t0);
        REQUIRE(store->card().open("/S", mc::open_mkdir, t0) == mc::result_ok);
    }
    for (const char* name : {"b.bin", "a.bin"}) {
        std::ofstream(folder / "S" / name, std::ios::binary) << "xyz";
    }
    std::ofstream(folder / "S" / ".ignored") << "x";
    auto store = rt::open_folder_card_store(folder, true, t1);
    std::vector<mc::DirEntry> list;
    REQUIRE(store->card().get_dir("/S/*", 8, list) == 4);
    CHECK(names(list) == std::vector<std::string>{".", "..", "a.bin", "b.bin"});
    CHECK(list[2].size == 3);
    CHECK(list[2].modified == t0); // the directory's own time, not the host's
}

TEST_CASE("memcard folder: a format erases the folder, keeping backups", "[memcard][folder]") {
    TempDir dir;
    const fs::path folder = dir.path() / "card1";
    auto store = rt::open_folder_card_store(folder, true, t0);
    mc::MemoryCard& card = store->card();
    make_save(card, "/S", bytes(500, 1), t0);
    REQUIRE(card.unformat() == mc::result_ok);
    store->finish();
    CHECK_FALSE(fs::exists(folder / "S"));
    CHECK(file_bytes(folder / ".backup" / "S" / "data.bin.1") == bytes(500, 1));
    CHECK_FALSE(fs::exists(folder / ".dir.toml"));
    auto again = rt::open_folder_card_store(folder, true, t1);
    CHECK_FALSE(again->card().formatted()); // card.toml wins over the seed
}

TEST_CASE("memcard folder: a folder that cannot be a card is refused", "[memcard][folder]") {
    TempDir dir;
    std::ofstream(dir.path() / "file") << "x";
    CHECK_THROWS_AS(rt::open_folder_card_store(dir.path() / "file", true, t0), std::runtime_error);
    fs::create_directories(dir.path() / "card");
    std::ofstream(dir.path() / "card" / "card.toml") << "version = 99\n";
    CHECK_THROWS_AS(rt::open_folder_card_store(dir.path() / "card", true, t0), std::runtime_error);
}

TEST_CASE("paths: the user data directory follows the platform's convention", "[memcard][paths]") {
    namespace platform = dq8::core::platform;
    static std::string xdg;
    static std::string home;
    static std::string appdata;
    const auto env = [](std::string_view name) -> std::string_view {
        if (name == "XDG_DATA_HOME") {
            return xdg;
        }
        if (name == "HOME") {
            return home;
        }
        if (name == "APPDATA") {
            return appdata;
        }
        return {};
    };
    xdg.clear();
    home.clear();
    appdata.clear();
    CHECK_THROWS(platform::user_data_dir_from(env));
#if defined(_WIN32)
    appdata = "C:\\Users\\u\\AppData\\Roaming";
    CHECK(platform::user_data_dir_from(env) == fs::path(appdata) / "DQ8-Native");
#elif defined(__APPLE__)
    home = "/Users/u";
    CHECK(platform::user_data_dir_from(env) == fs::path("/Users/u/Library/Application Support/DQ8-Native"));
#else
    home = "/home/u";
    CHECK(platform::user_data_dir_from(env) == fs::path("/home/u/.local/share/dq8-native"));
    xdg = "relative/ignored";
    CHECK(platform::user_data_dir_from(env) == fs::path("/home/u/.local/share/dq8-native"));
    xdg = "/data";
    CHECK(platform::user_data_dir_from(env) == fs::path("/data/dq8-native"));
#endif
}
