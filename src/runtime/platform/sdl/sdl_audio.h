// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/platform/audio.h"

#include <cstdint>
#include <memory>

// The SDL3 audio sink (architecture §7.1, §12.2; ADR-0005, ADR-0014): a 48 kHz stereo S16 device stream whose
// callback, on SDL's audio thread, pops AudioRing frames. Nothing flows back to the machine: the sink keeps
// the ring near its target latency by playing up to 0.5 % faster or slower
// (SDL_SetAudioStreamFrequencyRatio), skips the backlog when the ring holds more than four times the target
// (after a stall, or the silence the SPU2 writes at its first use), plays silence when the ring runs dry (an
// underrun), and reopens the device when its callbacks stop for longer than `reopen_after_ms` (a lost device,
// or one that never opened). start() and stop() are for the main thread; stop() is idempotent.
namespace dq8::rt {

struct SdlAudioConfig {
    std::uint32_t latency_ms = 64;        // target ring fill
    std::uint32_t reopen_after_ms = 1000; // silence from the device before it is reopened
};

class SdlAudioSink : public IAudioSink {
  public:
    // Frames skipped to catch up with a backlog; device reopens; the current frequency ratio.
    virtual std::uint64_t skipped() const = 0;
    virtual std::uint64_t reopens() const = 0;
    virtual float ratio() const = 0;
    // Tests: pause the device as if it had been lost (its callbacks stop).
    virtual void simulate_device_loss() = 0;
};

std::unique_ptr<SdlAudioSink> make_sdl_audio_sink(const SdlAudioConfig& config = {});

} // namespace dq8::rt
