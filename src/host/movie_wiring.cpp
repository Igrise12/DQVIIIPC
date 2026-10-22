// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// M4 session C: the movie HLE (ADR-0015). --movies play and nodecode, and auto when dq8-movie loads, build
// the sceMpeg HLE with its decoder (the library, or black pictures for nodecode) and install its overrides at
// the profile's [movie.sce_mpeg] addresses. play without the library is a start-up failure (exit 3); auto
// without it skips the movies (run_machine.cpp installs the skip overrides through
// game::resolve_movie_policy) and says why once, as a `movie_decoder` event; so does auto while a sound
// module is still a null server (game::movie_audio_problem), since the movies' audio could not drain.
#include "game/movies.h"
#include "host/m4_wiring.h"
#include "host/run.h"
#include "runtime/movie/movie_decoder.h"
#include "runtime/movie/sce_mpeg_hle.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace dq8::host {

namespace {

// Emits `movie_decoder {available: false, reason}` from the main thread before the machine thread starts.
class MovieDecoderReport final : public rt::IDevice {
  public:
    explicit MovieDecoderReport(std::string reason) : reason_(std::move(reason)) {}
    std::string_view name() const override { return "movie_decoder_report"; }
    void attach(rt::MachineServices& machine) override { machine_ = &machine; }
    void reset() override {}
    void emit() const {
        if (machine_ != nullptr) {
            machine_->emit(
                rt::MachineEvent{"movie_decoder", {}}.add("available", false).add("reason", reason_));
        }
    }

  private:
    std::string reason_;
    rt::MachineServices* machine_ = nullptr;
};

} // namespace

void wire_movies(M4Wiring& w) {
    const Options& o = w.options;
    if (o.movies != "play" && o.movies != "nodecode" && o.movies != "auto") {
        return; // skip, blank, stop: run_machine.cpp's player-entry overrides
    }
    const rt::MovieLibraryConfig library{o.movie_decoder ? o.movie_decoder->string() : std::string()};
    std::function<std::unique_ptr<rt::IMovieDecoder>()> make_decoder;
    std::optional<std::string> audio_problem;
    if (o.movies == "auto" && w.profile != nullptr) {
        audio_problem = game::movie_audio_problem(*w.profile);
    }
    if (o.movies == "nodecode") {
        make_decoder = [] { return rt::make_blank_movie_decoder(); };
    } else if (const auto problem = audio_problem ? audio_problem : rt::movie_library_problem(library)) {
        if (o.movies == "play") {
            throw BackendError("--movies play: " + *problem);
        }
        auto report = std::make_unique<MovieDecoderReport>(*problem);
        const MovieDecoderReport* r = report.get();
        w.devices.push_back(std::move(report));
        w.on_start.emplace_back([r] { r->emit(); });
        return;
    } else {
        make_decoder = [library] { return rt::make_library_movie_decoder(library); };
    }
    if (w.profile == nullptr || !w.profile->movie.sce_mpeg.present()) {
        return; // developer mode (--elf) or a profile without [movie.sce_mpeg]: nothing to hook
    }

    rt::SceMpegHleConfig config;
    config.make_decoder = std::move(make_decoder);
    config.video_callback = w.profile->movie.sce_mpeg.video_stream_callback;
    auto hle = std::make_unique<rt::SceMpegHle>(std::move(config));
    for (rt::OverrideSpec& spec : game::movie_hle_overrides(*w.profile, *hle)) {
        w.overrides.push_back(std::move(spec));
    }
    const rt::SceMpegHle* h = hle.get();
    w.devices.push_back(std::move(hle));
    if (!o.json) {
        // Pictures no decoder could paint are black; say so once at the end (never in the event log, so play
        // and nodecode logs stay comparable).
        w.on_stop.emplace_back([h] {
            const rt::SceMpegHle::Stats& s = h->stats();
            if (s.decoder_errors != 0) {
                std::fprintf(stderr,
                             "dq8-game: movies: %llu of %llu pictures were painted black (decoder errors)\n",
                             static_cast<unsigned long long>(s.decoder_errors),
                             static_cast<unsigned long long>(s.pictures));
            }
        });
    }
}

} // namespace dq8::host
