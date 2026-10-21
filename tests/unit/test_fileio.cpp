// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_stop.h"
#include "support/sif_client.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

namespace rt = dq8::rt;
using dq8::test::IopRig;
using dq8::test::SifClient;

namespace {

// The game's file library, as far as these tests need it: a setup call with two completion records, then
// requests whose results come back as records and system command 0x80000011.
class FileClient {
  public:
    static constexpr std::uint32_t client = 0x00012000;
    static constexpr std::uint32_t send = 0x00030000;
    static constexpr std::uint32_t recv = 0x00031000;
    static constexpr std::uint32_t records = 0x00034000; // two records of 0x440 bytes
    static constexpr std::uint32_t result_word = 0x00035000;
    static constexpr std::uint32_t semaphore = 7;

    explicit FileClient(SifClient& c) : c_(c) {}

    void setup() {
        REQUIRE(c_.bind(client, 0x80000001u) != 0);
        c_.write32(send, records);
        c_.write32(send + 4, records + 0x440);
        REQUIRE(c_.call(client, 0xFF, send, 8, recv, 8));
        version = c_.read32(recv);
        record_mode = c_.read32(recv + 4);
    }

    std::int32_t request(std::uint32_t fno, std::vector<std::uint8_t> req) {
        SifClient::put(req, 0x0, semaphore);
        SifClient::put(req, 0x4, result_word);
        SifClient::put(req, 0x8, 4);
        c_.write32(result_word, 0xDEADBEEF);
        const auto before = c_.fileio_completions().size();
        c_.write(send, req);
        sent_at = c_.machine().clock().now();
        REQUIRE(c_.call(client, fno, send, static_cast<std::uint32_t>(req.size()), recv, 4));
        CHECK(c_.read32(recv) != 0); // accepted
        REQUIRE(c_.wait_for([&] { return c_.fileio_completions().size() > before; }));
        // What the EE's completion handler does with the record.
        last_record = records + 0x440 * c_.fileio_completions().back();
        CHECK(c_.read32(last_record + 0x00) == semaphore);
        CHECK(c_.read32(last_record + 0x04) == fno);
        const std::uint32_t size = c_.read32(last_record + 0x0C);
        REQUIRE(size == 4);
        c_.write(c_.read32(last_record + 0x08), c_.read(last_record + 0x10, size));
        return static_cast<std::int32_t>(c_.read32(result_word));
    }

    std::int32_t open(const std::string& path) {
        std::vector<std::uint8_t> req(0x418, 0);
        SifClient::put(req, 0x0C, 1);
        std::memcpy(req.data() + 0x14, path.data(), path.size());
        return request(0, req);
    }
    std::int32_t close(std::int32_t fd) {
        std::vector<std::uint8_t> req(0x14, 0);
        SifClient::put(req, 0x0C, static_cast<std::uint32_t>(fd));
        return request(1, req);
    }
    std::int32_t read(std::int32_t fd, std::uint32_t buffer, std::uint32_t size) {
        std::vector<std::uint8_t> req(0x20, 0);
        SifClient::put(req, 0x0C, static_cast<std::uint32_t>(fd));
        SifClient::put(req, 0x10, buffer);
        SifClient::put(req, 0x14, size);
        const std::int32_t n = request(2, req);
        CHECK(c_.read32(last_record + 0x14) == 0); // no unaligned head
        CHECK(c_.read32(last_record + 0x18) == 0); // no unaligned tail
        return n;
    }
    std::int32_t lseek(std::int32_t fd, std::int32_t offset, std::uint32_t whence) {
        std::vector<std::uint8_t> req(0x1C, 0);
        SifClient::put(req, 0x0C, static_cast<std::uint32_t>(fd));
        SifClient::put(req, 0x10, static_cast<std::uint32_t>(offset));
        SifClient::put(req, 0x14, whence);
        return request(4, req);
    }

    std::uint32_t version = 0;
    std::uint32_t record_mode = 0;
    std::uint32_t last_record = 0;
    rt::Cycles sent_at = 0;

  private:
    SifClient& c_;
};

} // namespace

TEST_CASE("fileio: setup answers the protocol version and the two-record mode", "[iop][fileio]") {
    IopRig rig;
    rig.client.init_rpc();
    FileClient f(rig.client);
    f.setup();
    CHECK(f.version == 0x30303033u);
    CHECK(f.record_mode == 2);
}

TEST_CASE("fileio: open, lseek and an unaligned read return the disc's bytes", "[iop][fileio]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    FileClient f(c);
    f.setup();
    const auto title = dq8::test::seeded_bytes(3, 5000);

    const std::int32_t fd = f.open("cdrom0:\\BIN\\TITLE.BIN;1");
    REQUIRE(fd >= 0);
    CHECK(f.lseek(fd, 0, 2) == 5000);
    CHECK(f.lseek(fd, 0, 0) == 0);

    const std::uint32_t buffer = 0x00040003; // deliberately unaligned
    c.write(buffer - 3, std::vector<std::uint8_t>(5010, 0xA5));
    CHECK(f.read(fd, buffer, 5000) == 5000);
    CHECK(c.machine().clock().now() >= f.sent_at + c.machine().cost().cdvd_read(5000));
    CHECK(c.read(buffer, 5000) == title);
    CHECK(c.read(buffer - 3, 3) == std::vector<std::uint8_t>(3, 0xA5));
    CHECK(c.read(buffer + 5000, 7) == std::vector<std::uint8_t>(7, 0xA5));

    // A read across a sector boundary, then one at the end of the file.
    CHECK(f.lseek(fd, 2040, 0) == 2040);
    CHECK(f.read(fd, 0x00050001, 20) == 20);
    CHECK(c.read(0x00050001, 20) == std::vector<std::uint8_t>(title.begin() + 2040, title.begin() + 2060));
    CHECK(f.lseek(fd, -10, 2) == 4990);
    CHECK(f.read(fd, 0x00050101, 100) == 10);
    CHECK(f.read(fd, 0x00050101, 100) == 0);
    CHECK(f.lseek(fd, -1, 0) == -22);
    CHECK(f.lseek(fd, 0, 7) == -22);

    CHECK(f.close(fd) == 0);
    CHECK(f.read(fd, 0x00050101, 4) == -9);
    CHECK(f.close(fd) == -9);
}

TEST_CASE("fileio: disc paths accept separators, case and versions; other devices do not exist",
          "[iop][fileio]") {
    IopRig rig;
    rig.client.init_rpc();
    FileClient f(rig.client);
    f.setup();
    for (const std::string path :
         {"cdrom0:\\BIN\\TITLE.BIN;1", "cdrom0:/bin/title.bin", "cdrom0:\\DATA.HD6", "cdrom:\\data.hd6;1"}) {
        INFO(path);
        const std::int32_t fd = f.open(path);
        CHECK(fd >= 3);
        CHECK(f.close(fd) == 0);
    }
    CHECK(f.open("cdrom0:\\BIN\\NOPE.BIN;1") == -2);
    CHECK(f.open("cdrom0:\\BIN") == -2);
    CHECK(f.open("host0:TITLE.BIN") == -19);
    CHECK(f.open("TITLE.BIN") == -19);
}

TEST_CASE("fileio: completions alternate between the two records", "[iop][fileio]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    FileClient f(c);
    f.setup();
    const std::int32_t fd = f.open("cdrom0:\\DATA.HD6;1");
    f.lseek(fd, 0, 2);
    f.close(fd);
    REQUIRE(c.fileio_completions().size() == 3);
    CHECK(c.fileio_completions()[0] == 0);
    CHECK(c.fileio_completions()[1] == 1);
    CHECK(c.fileio_completions()[2] == 0);
}

TEST_CASE("fileio: a request before setup stops", "[iop][fileio]") {
    IopRig rig;
    SifClient& c = rig.client;
    c.init_rpc();
    REQUIRE(c.bind(FileClient::client, 0x80000001u) != 0);
    c.call(FileClient::client, 0, FileClient::send, 0x418, FileClient::recv, 4, false);
    CHECK_THROWS_AS(c.wait_for([] { return false; }), rt::GuestStop);
}
