// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The SDL3 audio sink (platform/sdl/sdl_audio.h) on SDL's dummy audio driver, which consumes audio at the
// real rate without a sound card: it plays what the ring holds, counts underruns once the ring is dry, skips
// a backlog, keeps its frequency ratio within ±0.5 %, reopens a device whose callbacks stop, and stops twice.
#include "runtime/platform/audio.h"
#include "runtime/platform/sdl/sdl_audio.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>

using namespace dq8::rt;

namespace {

void use_dummy_driver() {
#if defined(_WIN32)
    _putenv_s("SDL_AUDIO_DRIVER", "dummy");
#else
    ::setenv("SDL_AUDIO_DRIVER", "dummy", 1);
#endif
}

// Polls `done` for up to `seconds` of wall time.
bool eventually(const std::function<bool()>& done, double seconds = 5.0) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

void fill(AudioRing& ring, std::size_t frames) {
    const std::vector<StereoFrame> block(frames, StereoFrame{1000, -1000});
    REQUIRE(ring.push(block) == frames);
}

} // namespace

TEST_CASE("sdl audio sink: plays the ring, then counts underruns; stop is idempotent", "[audio_sdl]") {
    use_dummy_driver();
    AudioRing ring(1u << 14);
    fill(ring, 2400);
    SdlAudioConfig config;
    config.latency_ms = 20;
    auto sink = make_sdl_audio_sink(config);
    sink->start(ring);
    CHECK(eventually([&] { return sink->stats().frames_played == 2400; }));
    CHECK(eventually([&] { return sink->stats().underruns > 0; }));
    CHECK(ring.available() == 0);
    CHECK(sink->ratio() >= 0.995f);
    CHECK(sink->ratio() <= 1.005f);
    sink->stop();
    sink->stop();
    const auto played = sink->stats().frames_played;
    CHECK(played + sink->skipped() == 2400);
}

TEST_CASE("sdl audio sink: skips a backlog and steers the rate within half a percent", "[audio_sdl]") {
    use_dummy_driver();
    AudioRing ring(1u << 16);
    fill(ring, 48'000); // one second against a 20 ms target
    SdlAudioConfig config;
    config.latency_ms = 20;
    auto sink = make_sdl_audio_sink(config);
    sink->start(ring);
    CHECK(eventually([&] { return sink->skipped() > 40'000; }));
    // Keep the ring above its target: the sink plays faster, never by more than 0.5 %.
    bool faster = false;
    for (int i = 0; i < 60 && !faster; ++i) {
        if (ring.available() < 1'800) {
            fill(ring, 960);
        }
        faster = sink->ratio() > 1.0f;
        CHECK(sink->ratio() <= 1.005f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(faster);
    sink->stop();
}

TEST_CASE("sdl audio sink: reopens a device whose callbacks stop", "[audio_sdl]") {
    use_dummy_driver();
    AudioRing ring(1u << 14);
    SdlAudioConfig config;
    config.latency_ms = 20;
    config.reopen_after_ms = 200;
    auto sink = make_sdl_audio_sink(config);
    sink->start(ring);
    CHECK(eventually([&] { return sink->stats().underruns > 0; }));
    CHECK(sink->reopens() == 0);
    sink->simulate_device_loss();
    CHECK(eventually([&] { return sink->reopens() >= 1; }));
    const auto before = sink->stats().underruns;
    CHECK(eventually([&] { return sink->stats().underruns > before; }));
    fill(ring, 480);
    CHECK(eventually([&] { return sink->stats().frames_played == 480; }));
    sink->stop();
}
