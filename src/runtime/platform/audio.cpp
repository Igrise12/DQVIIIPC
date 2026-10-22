// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/platform/audio.h"

#include "runtime/machine/clock.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <stdexcept>
#include <string>
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

// A canonical 44-byte RIFF/WAVE header for 48 kHz stereo 16-bit PCM; the sizes are patched on finish().
class WavTap final : public IAudioTap {
  public:
    explicit WavTap(const std::string& path) : path_(path), file_(std::fopen(path.c_str(), "wb")) {
        if (file_ == nullptr) {
            throw std::runtime_error("cannot write the audio dump '" + path + "'");
        }
        write_header(0);
    }
    WavTap(const WavTap&) = delete;
    WavTap& operator=(const WavTap&) = delete;
    ~WavTap() override {
        try {
            finish();
        } catch (...) { // a destructor reports nothing; finish() is called explicitly at the end of a run
        }
    }

    void write(std::span<const StereoFrame> frames) override {
        if (file_ == nullptr) {
            return;
        }
        std::array<std::uint8_t, 4 * 1024> bytes{};
        for (std::size_t done = 0; done < frames.size();) {
            const std::size_t n = std::min<std::size_t>(1024, frames.size() - done);
            for (std::size_t i = 0; i < n; ++i) {
                put16(bytes.data() + i * 4, static_cast<std::uint16_t>(frames[done + i].left));
                put16(bytes.data() + i * 4 + 2, static_cast<std::uint16_t>(frames[done + i].right));
            }
            if (std::fwrite(bytes.data(), 1, n * 4, file_) != n * 4) {
                failed_ = true;
            }
            done += n;
        }
        frames_ += frames.size();
    }

    void finish() override {
        if (file_ == nullptr) {
            return;
        }
        // RIFF sizes are 32-bit: a dump longer than about six hours keeps its data but not its length.
        const std::uint64_t data = std::min<std::uint64_t>(frames_ * 4, 0xFFFFFFFFu - 36);
        if (std::fseek(file_, 0, SEEK_SET) != 0) {
            failed_ = true;
        } else {
            write_header(static_cast<std::uint32_t>(data));
        }
        if (std::fclose(file_) != 0) {
            failed_ = true;
        }
        file_ = nullptr;
        if (failed_) {
            throw std::runtime_error("writing the audio dump '" + path_ + "' failed");
        }
    }

  private:
    static void put16(std::uint8_t* p, std::uint16_t v) {
        p[0] = static_cast<std::uint8_t>(v);
        p[1] = static_cast<std::uint8_t>(v >> 8);
    }
    static void put32(std::uint8_t* p, std::uint32_t v) {
        put16(p, static_cast<std::uint16_t>(v));
        put16(p + 2, static_cast<std::uint16_t>(v >> 16));
    }

    void write_header(std::uint32_t data_bytes) {
        std::array<std::uint8_t, 44> h{};
        const auto tag = [&h](std::size_t at, const char* four) {
            for (std::size_t i = 0; i < 4; ++i) {
                h[at + i] = static_cast<std::uint8_t>(four[i]);
            }
        };
        tag(0, "RIFF");
        put32(&h[4], 36 + data_bytes);
        tag(8, "WAVE");
        tag(12, "fmt ");
        put32(&h[16], 16);
        put16(&h[20], 1); // PCM
        put16(&h[22], 2); // channels
        put32(&h[24], spu2_sample_rate);
        put32(&h[28], spu2_sample_rate * 4); // bytes per second
        put16(&h[32], 4);                    // bytes per frame
        put16(&h[34], 16);                   // bits per sample
        tag(36, "data");
        put32(&h[40], data_bytes);
        if (std::fwrite(h.data(), 1, h.size(), file_) != h.size()) {
            failed_ = true;
        }
    }

    std::string path_;
    std::FILE* file_ = nullptr;
    std::uint64_t frames_ = 0;
    bool failed_ = false;
};

} // namespace

std::unique_ptr<IAudioSink> make_null_audio_sink() {
    return std::make_unique<NullAudioSink>();
}

std::unique_ptr<IAudioTap> make_wav_tap(const std::string& path) {
    return std::make_unique<WavTap>(path);
}

} // namespace dq8::rt
