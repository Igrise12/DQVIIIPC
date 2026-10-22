// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// M4 session B: the audio output. AudioOutput with its taps (--audio-dump), the SPU2 on the IOP bus, the SDL3
// or null sink started and stopped around the machine thread, the `audio {frames, hash}` events (once per
// vblank in which frames were mixed), --dump-spu2-at, and the `audio_hash` field of the end event. Nothing
// the sink does reaches the machine.
#include "dq8/core/error.h"
#include "dq8/core/hash.h"
#include "host/m4_wiring.h"
#include "host/run.h"
#include "runtime/machine/clock.h"
#include "runtime/platform/audio.h"
#include "runtime/spu2/spu2.h"
#ifdef DQ8_HAVE_WINDOW
#include "runtime/platform/sdl/sdl_audio.h"
#endif

#include <algorithm>
#include <bit>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dq8::host {

namespace {

// On the machine thread at every vblank start: brings the SPU2 up to the vblank (the IOP never runs past an
// event, so this is a time it has reached), reports the frames mixed so far, and writes the requested sound
// RAM dumps. Attached after the SPU2; adds no event, and reports nothing while the SPU2 is unused.
class AudioReporter final : public rt::IDevice, public rt::IVblankListener {
  public:
    AudioReporter(rt::Spu2& spu2, rt::AudioOutput& output, std::vector<AtVblank> dumps)
        : spu2_(spu2), output_(output), dumps_(std::move(dumps)) {}

    std::string_view name() const override { return "audio-report"; }
    void attach(rt::MachineServices& machine) override {
        machine_ = &machine;
        machine.add_vblank_listener(rt::VblankPhase::intc, *this);
    }
    void reset() override { reported_ = 0; }

    void on_vblank_start(std::uint64_t field) override {
        spu2_.advance_to(machine_->video().field_start(field));
        const std::uint64_t vblank = field + 1; // vblanks started, as --dump-ram-at counts them
        for (const AtVblank& d : dumps_) {
            if (d.vblank == vblank) {
                const std::vector<std::uint8_t> ram = spu2_.ram_bytes_le();
                std::ofstream out(d.path, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(ram.data()),
                          static_cast<std::streamsize>(ram.size()));
                if (!out) {
                    throw core::Error(core::ErrorCode::io_write,
                                      "cannot write the SPU2 dump " + d.path.string());
                }
                machine_->emit(
                    rt::MachineEvent{"spu2_dump", {}}.add("sha1", core::to_hex(core::Sha1::of(ram))));
            }
        }
        const std::uint64_t frames = output_.frames();
        if (frames != reported_) {
            reported_ = frames;
            machine_->emit(
                rt::MachineEvent{"audio", {}}.add("frames", frames).add("hash", output_.hash_hex()));
        }
    }

  private:
    rt::Spu2& spu2_;
    rt::AudioOutput& output_;
    std::vector<AtVblank> dumps_;
    rt::MachineServices* machine_ = nullptr;
    std::uint64_t reported_ = 0;
};

} // namespace

void wire_audio(M4Wiring& w) {
    const Options& o = w.options;
    // The ring holds at least four times the sink's target, so that the sink can steer its fill level.
    const std::size_t latency_frames = std::size_t{o.audio_latency_ms} * rt::spu2_sample_rate / 1000;
    const std::size_t ring = std::bit_ceil(std::max<std::size_t>(16'384, 4 * latency_frames));
    auto output = std::make_shared<rt::AudioOutput>(ring);
    if (o.audio_dump) {
        try {
            output->add_tap(rt::make_wav_tap(o.audio_dump->string()));
        } catch (const std::runtime_error& e) {
            throw core::Error(core::ErrorCode::io_write, e.what());
        }
    }

    std::shared_ptr<rt::IAudioSink> sink;
    std::function<void()> report_sink;
    if (o.audio == "sdl") {
#ifdef DQ8_HAVE_WINDOW
        rt::SdlAudioConfig config;
        config.latency_ms = o.audio_latency_ms;
        std::shared_ptr<rt::SdlAudioSink> sdl = rt::make_sdl_audio_sink(config);
        if (!o.json) {
            report_sink = [sdl] {
                const rt::IAudioSink::Stats s = sdl->stats();
                std::fprintf(stderr,
                             "audio: %llu frames played, %llu underruns, %llu skipped, %llu reopens\n",
                             static_cast<unsigned long long>(s.frames_played),
                             static_cast<unsigned long long>(s.underruns),
                             static_cast<unsigned long long>(sdl->skipped()),
                             static_cast<unsigned long long>(sdl->reopens()));
            };
        }
        sink = std::move(sdl);
#else
        throw BackendError("--audio sdl: this build has no SDL3 (DQ8_BUILD_WINDOW=OFF)");
#endif
    } else {
        sink = rt::make_null_audio_sink();
    }

    auto spu2 = std::make_unique<rt::Spu2>(rt::Spu2Config{output.get()});
    w.iop.devices.push_back(spu2->mapping());
    auto reporter = std::make_unique<AudioReporter>(*spu2, *output, o.dump_spu2_at);
    w.devices.push_back(std::move(spu2));
    w.devices.push_back(std::move(reporter));

    w.on_start.push_back([sink, output] { sink->start(output->ring()); });
    // Stop the sink (it reads the ring), then flush the taps.
    w.on_stop.push_back([sink, output, report_sink] {
        sink->stop();
        output->finish();
        if (report_sink) {
            report_sink();
        }
    });
    w.end_fields.push_back([output](rt::MachineEvent& end) { end.add("audio_hash", output->hash_hex()); });
}

} // namespace dq8::host
