// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The audio output path (platform/audio.h): the lock-free ring between the machine thread and the audio
// thread, AudioOutput's audio hash, which must not depend on how writes are split, and the WAV tap, whose
// file does not either.
#include "runtime/platform/audio.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace dq8::rt;

namespace {

std::vector<StereoFrame> ramp(std::size_t n, int start = 0) {
    std::vector<StereoFrame> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i].left = static_cast<std::int16_t>(start + static_cast<int>(i));
        out[i].right = static_cast<std::int16_t>(-(start + static_cast<int>(i)));
    }
    return out;
}

} // namespace

TEST_CASE("audio ring: frames come out in order, wrap around, and overflow is dropped and counted",
          "[audio]") {
    CHECK_THROWS_AS(AudioRing(1000), std::invalid_argument);
    AudioRing ring(8);
    const auto a = ramp(6);
    CHECK(ring.push(a) == 6);
    std::vector<StereoFrame> out(4);
    CHECK(ring.pop(out) == 4);
    CHECK(out[3].left == 3);
    const auto b = ramp(10, 100);
    CHECK(ring.push(b) == 6); // 2 left + 6 free
    CHECK(ring.dropped() == 4);
    CHECK(ring.available() == 8);
    std::vector<StereoFrame> all(16);
    CHECK(ring.pop(all) == 8);
    CHECK(all[0].left == 4);
    CHECK(all[2].left == 100); // across the wrap
    CHECK(all[7].left == 105);
    CHECK(ring.pop(all) == 0);
}

TEST_CASE("audio ring: one producer and one consumer thread see every frame once", "[audio]") {
    AudioRing ring(1024);
    constexpr int total = 200'000;
    std::thread producer([&] {
        int next = 0;
        while (next < total) {
            const auto chunk = ramp(static_cast<std::size_t>(std::min(97, total - next)), next);
            next += static_cast<int>(ring.push(chunk));
        }
    });
    int expected = 0;
    bool in_order = true;
    std::vector<StereoFrame> buf(61);
    while (expected < total) {
        const std::size_t n = ring.pop(buf);
        for (std::size_t i = 0; i < n; ++i) {
            in_order = in_order && buf[i].left == static_cast<std::int16_t>(expected);
            ++expected;
        }
    }
    producer.join();
    CHECK(in_order);
}

TEST_CASE("audio output: the hash is the same for one write and for many small ones", "[audio]") {
    const auto frames = ramp(5000);
    AudioOutput one(64);
    one.write(frames);
    AudioOutput many(64);
    for (std::size_t at = 0; at < frames.size(); at += 37) {
        const std::size_t n = std::min<std::size_t>(37, frames.size() - at);
        many.write(std::span<const StereoFrame>(frames.data() + at, n));
    }
    CHECK(one.frames() == 5000);
    CHECK(many.frames() == 5000);
    CHECK(one.hash_hex() == many.hash_hex());
    CHECK(one.hash_hex().size() == 40);
    CHECK(one.ring().dropped() ==
          5000 - 64); // nobody popped: the ring keeps the oldest 64 and drops the rest
    AudioOutput empty;
    CHECK(empty.hash_hex() != one.hash_hex());
    auto sink = make_null_audio_sink();
    sink->start(one.ring());
    sink->stop();
    CHECK(sink->stats().frames_played == 0);
}

namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return std::uint32_t{b[at]} | std::uint32_t{b[at + 1]} << 8 | std::uint32_t{b[at + 2]} << 16 |
           std::uint32_t{b[at + 3]} << 24;
}

} // namespace

TEST_CASE("wav tap: a 48 kHz stereo 16-bit file, the same however the writes are split", "[audio]") {
    const auto dir = std::filesystem::temp_directory_path() / "dq8-test-wav-tap";
    std::filesystem::create_directories(dir);
    const auto frames = ramp(3000, -1500);
    {
        AudioOutput one(64);
        one.add_tap(make_wav_tap((dir / "one.wav").string()));
        one.write(frames);
        one.finish();
        AudioOutput many(64);
        many.add_tap(make_wav_tap((dir / "many.wav").string()));
        for (std::size_t at = 0; at < frames.size(); at += 113) {
            many.write(std::span<const StereoFrame>(frames.data() + at,
                                                    std::min<std::size_t>(113, frames.size() - at)));
        }
        many.finish();
        many.finish(); // finishing twice is harmless
    }
    const auto a = read_file(dir / "one.wav");
    const auto b = read_file(dir / "many.wav");
    REQUIRE(a.size() == 44 + 3000 * 4);
    CHECK(a == b);
    CHECK(std::string(a.begin(), a.begin() + 4) == "RIFF");
    CHECK(le32(a, 4) == 36 + 3000 * 4);
    CHECK(std::string(a.begin() + 8, a.begin() + 16) == "WAVEfmt ");
    CHECK(le32(a, 24) == 48'000);
    CHECK(le32(a, 28) == 192'000);
    CHECK(le32(a, 40) == 3000 * 4);
    // Frame 2 is (-1498, 1498), little-endian left then right.
    CHECK(a[44 + 8] == static_cast<std::uint8_t>(static_cast<std::uint16_t>(-1498)));
    CHECK(a[44 + 11] == static_cast<std::uint8_t>(1498 >> 8));
    CHECK_THROWS_AS(make_wav_tap((dir / "missing" / "x.wav").string()), std::runtime_error);
    std::filesystem::remove_all(dir);
}
