// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/hash.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

// The audio output path (architecture §7.1, §12.2; ADR-0014; fixed on M4 day 0):
//
//   SPU2 (machine thread, virtual time) -> AudioOutput::write()
//      -> taps, lossless and deterministic: the streaming SHA-1 audio hash, an optional WAV writer
//      -> AudioRing::push(), never blocks, drops what does not fit
//         -> the sink's audio thread (SDL's device callback) pops it
//
// Nothing flows back: the guest never observes the ring, the sink or host audio timing. With --speed max the
// ring fills and blocks are dropped (counted for the summary, never in events); a stalled machine makes the
// sink play silence. The ring, AudioOutput and the null sink are day 0; the WAV tap and the SDL3 sink are M4
// session B.
namespace dq8::rt {

// One 48 kHz stereo sample.
struct StereoFrame {
    std::int16_t left = 0;
    std::int16_t right = 0;
};

// Single producer (the machine thread), single consumer (the audio thread). Capacity is a power of two.
class AudioRing {
  public:
    explicit AudioRing(std::size_t capacity_frames);
    AudioRing(const AudioRing&) = delete;
    AudioRing& operator=(const AudioRing&) = delete;

    std::size_t capacity() const noexcept { return frames_.size(); }
    // Producer: copies as many frames as fit and returns that count; the rest are dropped and counted.
    std::size_t push(std::span<const StereoFrame> frames) noexcept;
    // Consumer: fills `out` from the oldest frames and returns how many it had.
    std::size_t pop(std::span<StereoFrame> out) noexcept;
    // Frames waiting (a snapshot; either side may call it).
    std::size_t available() const noexcept;
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

  private:
    std::vector<StereoFrame> frames_;
    std::size_t mask_ = 0;
    std::atomic<std::uint64_t> head_{0}; // next frame to write (producer)
    std::atomic<std::uint64_t> tail_{0}; // next frame to read (consumer)
    std::atomic<std::uint64_t> dropped_{0};
};

// Sees every frame the machine produces, in order, on the machine thread.
class IAudioTap {
  public:
    virtual ~IAudioTap() = default;
    virtual void write(std::span<const StereoFrame> frames) = 0;
    // The run ended: flush.
    virtual void finish() {}
};

// What the SPU2 writes to. Owned by the host; lives on the machine thread except for ring().
class AudioOutput {
  public:
    explicit AudioOutput(std::size_t ring_frames = 16'384);

    void add_tap(std::unique_ptr<IAudioTap> tap);
    void write(std::span<const StereoFrame> frames);
    void finish();

    AudioRing& ring() noexcept { return ring_; }
    // Frames written so far and the SHA-1 of their bytes (little-endian left, right): the audio hash of a
    // run, identical whatever the sink, ring size or speed.
    std::uint64_t frames() const noexcept { return frames_; }
    std::string hash_hex() const;

  private:
    AudioRing ring_;
    std::vector<std::unique_ptr<IAudioTap>> taps_;
    core::Sha1 sha_;
    std::uint64_t frames_ = 0;
};

// Where the ring's frames go. Called from the main thread only.
class IAudioSink {
  public:
    struct Stats {
        std::uint64_t underruns = 0; // callbacks that found too few frames
        std::uint64_t frames_played = 0;
    };
    virtual ~IAudioSink() = default;
    virtual void start(AudioRing& ring) = 0;
    // Idempotent; called before the AudioOutput goes away.
    virtual void stop() = 0;
    virtual Stats stats() const = 0;
};

// Plays nothing: the headless default (the hash and the taps still run).
std::unique_ptr<IAudioSink> make_null_audio_sink();

// M4 session B: a 48 kHz stereo 16-bit WAV writer (platform/audio.cpp); the SDL3 sink is
// make_sdl_audio_sink() in platform/sdl/sdl_audio.h.
std::unique_ptr<IAudioTap> make_wav_tap(const std::string& path);

} // namespace dq8::rt
