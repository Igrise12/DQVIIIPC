// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/platform/audio.h"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>
#include <utility>

namespace dq8::rt {

AudioRing::AudioRing(std::size_t capacity_frames) {
    if (capacity_frames == 0 || !std::has_single_bit(capacity_frames)) {
        throw std::invalid_argument("AudioRing: the capacity must be a power of two");
    }
    frames_.resize(capacity_frames);
    mask_ = capacity_frames - 1;
}

std::size_t AudioRing::push(std::span<const StereoFrame> frames) noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    const std::size_t space = frames_.size() - static_cast<std::size_t>(head - tail);
    const std::size_t n = std::min(space, frames.size());
    for (std::size_t i = 0; i < n; ++i) {
        frames_[static_cast<std::size_t>(head + i) & mask_] = frames[i];
    }
    head_.store(head + n, std::memory_order_release);
    if (n < frames.size()) {
        dropped_.fetch_add(frames.size() - n, std::memory_order_relaxed);
    }
    return n;
}

std::size_t AudioRing::pop(std::span<StereoFrame> out) noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    const std::size_t n = std::min(static_cast<std::size_t>(head - tail), out.size());
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = frames_[static_cast<std::size_t>(tail + i) & mask_];
    }
    tail_.store(tail + n, std::memory_order_release);
    return n;
}

std::size_t AudioRing::available() const noexcept {
    return static_cast<std::size_t>(head_.load(std::memory_order_acquire) -
                                    tail_.load(std::memory_order_acquire));
}

AudioOutput::AudioOutput(std::size_t ring_frames) : ring_(ring_frames) {}

void AudioOutput::add_tap(std::unique_ptr<IAudioTap> tap) {
    taps_.push_back(std::move(tap));
}

void AudioOutput::write(std::span<const StereoFrame> frames) {
    // The hash covers the frames as bytes in a fixed layout, independent of the host's struct padding.
    std::array<std::uint8_t, 4 * 256> bytes{};
    for (std::size_t done = 0; done < frames.size();) {
        const std::size_t n = std::min<std::size_t>(256, frames.size() - done);
        for (std::size_t i = 0; i < n; ++i) {
            const auto l = static_cast<std::uint16_t>(frames[done + i].left);
            const auto r = static_cast<std::uint16_t>(frames[done + i].right);
            bytes[i * 4 + 0] = static_cast<std::uint8_t>(l);
            bytes[i * 4 + 1] = static_cast<std::uint8_t>(l >> 8);
            bytes[i * 4 + 2] = static_cast<std::uint8_t>(r);
            bytes[i * 4 + 3] = static_cast<std::uint8_t>(r >> 8);
        }
        sha_.update(std::span<const std::uint8_t>(bytes.data(), n * 4));
        done += n;
    }
    for (const auto& tap : taps_) {
        tap->write(frames);
    }
    ring_.push(frames);
    frames_ += frames.size();
}

void AudioOutput::finish() {
    for (const auto& tap : taps_) {
        tap->finish();
    }
}

std::string AudioOutput::hash_hex() const {
    core::Sha1 copy = sha_;
    return core::to_hex(copy.finish());
}

namespace {

class NullAudioSink final : public IAudioSink {
  public:
    void start(AudioRing& /*ring*/) override {}
    void stop() override {}
    Stats stats() const override { return {}; }
};

} // namespace

std::unique_ptr<IAudioSink> make_null_audio_sink() {
    return std::make_unique<NullAudioSink>();
}

} // namespace dq8::rt
