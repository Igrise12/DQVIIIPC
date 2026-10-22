// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The SDL3 audio sink (sdl_audio.h).
#include "runtime/platform/sdl/sdl_audio.h"

#include "runtime/machine/clock.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace dq8::rt {

namespace {

constexpr int bytes_per_frame = 4;
constexpr float max_ratio_offset = 0.005f; // ±0.5 %
constexpr float ratio_hysteresis = 0.0005f;

class Sink final : public SdlAudioSink {
  public:
    explicit Sink(const SdlAudioConfig& config)
        : config_(config),
          target_(std::max<std::size_t>(1, std::size_t{config.latency_ms} * spu2_sample_rate / 1000)) {}
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    ~Sink() override { stop(); }

    void start(AudioRing& ring) override {
        std::lock_guard lock(mutex_);
        if (running_) {
            return;
        }
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            std::fprintf(stderr, "audio: SDL audio did not start (%s); playing nothing\n", SDL_GetError());
            return;
        }
        ring_ = &ring;
        running_ = true;
        open_locked();
        monitor_ = std::thread([this] { monitor(); });
    }

    void stop() override {
        {
            std::lock_guard lock(mutex_);
            if (!running_) {
                return;
            }
            running_ = false;
        }
        wake_.notify_all();
        if (monitor_.joinable()) {
            monitor_.join();
        }
        std::lock_guard lock(mutex_);
        close_locked();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        ring_ = nullptr;
    }

    Stats stats() const override {
        Stats s;
        s.underruns = underruns_.load(std::memory_order_relaxed);
        s.frames_played = played_.load(std::memory_order_relaxed);
        return s;
    }
    std::uint64_t skipped() const override { return skipped_.load(std::memory_order_relaxed); }
    std::uint64_t reopens() const override { return reopens_.load(std::memory_order_relaxed); }
    float ratio() const override { return ratio_.load(std::memory_order_relaxed); }
    std::string device_name() const override {
        std::lock_guard lock(mutex_);
        return device_name_;
    }
    bool device_found() const override {
        std::lock_guard lock(mutex_);
        return device_found_;
    }

    void simulate_device_loss() override {
        std::lock_guard lock(mutex_);
        if (stream_ != nullptr) {
            SDL_PauseAudioStreamDevice(stream_);
        }
    }

  private:
    static void SDLCALL callback(void* user, SDL_AudioStream* stream, int additional, int /*total*/) {
        static_cast<Sink*>(user)->feed(stream, additional);
    }

    // SDL's audio thread.
    void feed(SDL_AudioStream* stream, int additional) {
        last_callback_ns_.store(SDL_GetTicksNS(), std::memory_order_relaxed);
        if (additional <= 0 || ring_ == nullptr) {
            return;
        }
        AudioRing& ring = *ring_;
        std::size_t available = ring.available();
        if (available > 4 * target_) {
            std::size_t drop = available - target_;
            scratch_.resize(std::max<std::size_t>(scratch_.size(), 4096));
            while (drop > 0) {
                const std::size_t n = ring.pop(std::span(scratch_.data(), std::min(drop, scratch_.size())));
                if (n == 0) {
                    break;
                }
                skipped_.fetch_add(n, std::memory_order_relaxed);
                drop -= n;
            }
            available = ring.available();
        }
        // Proportional control of the fill level, within ±0.5 %.
        const float error = std::clamp((static_cast<float>(available) - static_cast<float>(target_)) /
                                           static_cast<float>(target_),
                                       -1.0f, 1.0f);
        const float ratio = 1.0f + max_ratio_offset * error;
        if (std::fabs(ratio - ratio_.load(std::memory_order_relaxed)) >= ratio_hysteresis) {
            if (SDL_SetAudioStreamFrequencyRatio(stream, ratio)) {
                ratio_.store(ratio, std::memory_order_relaxed);
            }
        }
        const auto frames = static_cast<std::size_t>((additional + bytes_per_frame - 1) / bytes_per_frame);
        if (buffer_.size() < frames) {
            buffer_.resize(frames);
        }
        const std::size_t got = ring.pop(std::span(buffer_.data(), frames));
        if (got < frames) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(got),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(frames), StereoFrame{});
            underruns_.fetch_add(1, std::memory_order_relaxed);
        }
        played_.fetch_add(got, std::memory_order_relaxed);
        SDL_PutAudioStreamData(stream, buffer_.data(), static_cast<int>(frames) * bytes_per_frame);
    }

    // The playback device named in the configuration, or the default one when it names none or no device
    // has that name (now: it is looked for again at every reopen).
    SDL_AudioDeviceID device_locked() {
        device_found_ = config_.device.empty();
        if (config_.device.empty()) {
            return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
        }
        SDL_AudioDeviceID found = 0;
        int count = 0;
        if (SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count)) {
            for (int i = 0; i < count && found == 0; ++i) {
                const char* name = SDL_GetAudioDeviceName(ids[i]);
                if (name != nullptr && config_.device == name) {
                    found = ids[i];
                }
            }
            SDL_free(ids);
        }
        if (found == 0) {
            if (!device_warned_) {
                std::fprintf(stderr, "audio: no playback device is called \"%s\"; using the default device\n",
                             config_.device.c_str());
                device_warned_ = true;
            }
            return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
        }
        device_found_ = true;
        return found;
    }

    void open_locked() {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(spu2_sample_rate)};
        last_open_ns_ = SDL_GetTicksNS();
        last_callback_ns_.store(last_open_ns_, std::memory_order_relaxed);
        stream_ = SDL_OpenAudioDeviceStream(device_locked(), &spec, &Sink::callback, this);
        if (stream_ == nullptr) {
            if (!warned_) {
                std::fprintf(stderr, "audio: no audio device (%s); retrying\n", SDL_GetError());
                warned_ = true;
            }
            return;
        }
        // The volume: a gain on the device side of the stream, after the hash and the taps.
        if (config_.volume != 100 &&
            !SDL_SetAudioStreamGain(
                stream_, static_cast<float>(std::min<std::uint32_t>(config_.volume, 100)) / 100.0f)) {
            std::fprintf(stderr, "audio: cannot set the volume: %s\n", SDL_GetError());
        }
        const char* name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream_));
        device_name_ = name != nullptr ? name : "";
        ratio_.store(1.0f, std::memory_order_relaxed);
        SDL_ResumeAudioStreamDevice(stream_);
    }

    void close_locked() {
        if (stream_ != nullptr) {
            SDL_DestroyAudioStream(stream_);
            stream_ = nullptr;
        }
    }

    // Reopens the device when its callbacks stopped (or it never opened).
    void monitor() {
        {
            std::unique_lock lock(mutex_);
            const std::uint64_t quiet_ns = std::uint64_t{config_.reopen_after_ms} * 1'000'000;
            while (running_) {
                wake_.wait_for(lock, std::chrono::milliseconds(50));
                if (!running_) {
                    break;
                }
                const std::uint64_t now = SDL_GetTicksNS();
                const std::uint64_t last = last_callback_ns_.load(std::memory_order_relaxed);
                if (now - last >= quiet_ns && now - last_open_ns_ >= quiet_ns) {
                    close_locked();
                    open_locked();
                    if (stream_ != nullptr) {
                        reopens_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
        // A reopen asks SDL for the device's name, which SDL keeps in thread-local storage; this thread is
        // not SDL's, so SDL cannot free it at exit.
        SDL_CleanupTLS();
    }

    SdlAudioConfig config_;
    std::size_t target_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread monitor_;
    bool running_ = false;
    bool warned_ = false;
    bool device_warned_ = false;
    bool device_found_ = false;
    std::string device_name_;
    SDL_AudioStream* stream_ = nullptr;
    std::uint64_t last_open_ns_ = 0;
    AudioRing* ring_ = nullptr;
    // The audio thread's own buffers.
    std::vector<StereoFrame> buffer_;
    std::vector<StereoFrame> scratch_;
    std::atomic<std::uint64_t> last_callback_ns_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> played_{0};
    std::atomic<std::uint64_t> skipped_{0};
    std::atomic<std::uint64_t> reopens_{0};
    std::atomic<float> ratio_{1.0f};
};

} // namespace

std::unique_ptr<SdlAudioSink> make_sdl_audio_sink(const SdlAudioConfig& config) {
    return std::make_unique<Sink>(config);
}

std::vector<std::string> sdl_audio_playback_devices() {
    std::vector<std::string> names;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return names;
    }
    int count = 0;
    if (SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count)) {
        for (int i = 0; i < count; ++i) {
            if (const char* name = SDL_GetAudioDeviceName(ids[i])) {
                names.emplace_back(name);
            }
        }
        SDL_free(ids);
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return names;
}

} // namespace dq8::rt
