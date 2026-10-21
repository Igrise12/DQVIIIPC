// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The audio output path of M4 day 0 (platform/audio.h): the lock-free ring between the machine thread and the
// audio thread, and AudioOutput's audio hash, which must not depend on how writes are split.
#include "runtime/ee/guest_stop.h"
#include "runtime/platform/audio.h"
#include "runtime/spu2/spu2.h"
#include "support/fake_iop_host.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
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

TEST_CASE("spu2 day-0 stub: maps on the IOP bus and stops on any access", "[audio][spu2]") {
    dq8::test::FakeMachine machine;
    dq8::test::FakeIopHost host(machine);
    AudioOutput out;
    Spu2 spu2(Spu2Config{&out});
    spu2.attach(machine);
    spu2.connect(host);
    const IopDeviceMapping m = spu2.mapping();
    CHECK(m.base == 0x1F900000u);
    CHECK(m.device == &spu2);
    CHECK(m.dma_channels == std::vector<std::uint32_t>{4, 7});
    std::uint32_t value = 0;
    CHECK_THROWS_AS(spu2.read(0x19A, 2, value, IopAccess{}), GuestStop);
    CHECK_THROWS_AS(spu2.dma_start(IopDmaRequest{4, 0, 0, 0, 0}), GuestStop);
    CHECK_FALSE(machine.events().next_time()); // the stub schedules nothing
}
