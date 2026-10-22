// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "game/movies.h"

#include "game/unit_specs.h"
#include "runtime/ee/context.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/machine/machine_services.h"
#include "runtime/movie/movie_decoder.h"
#include "runtime/movie/sce_mpeg_hle.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace dq8::game {

namespace {

constexpr unsigned reg_v0 = 2;

rt::MachineServices& machine_of(const rt::OverrideCall& call) {
    if (call.machine == nullptr) {
        throw std::logic_error("a movie override ran without a machine");
    }
    return *call.machine;
}

} // namespace

std::optional<MoviePolicy> parse_movie_policy(std::string_view text) noexcept {
    if (text == "skip") {
        return MoviePolicy::skip;
    }
    if (text == "blank") {
        return MoviePolicy::blank;
    }
    if (text == "stop") {
        return MoviePolicy::stop;
    }
    return std::nullopt;
}

std::string_view to_string(MoviePolicy policy) noexcept {
    switch (policy) {
    case MoviePolicy::skip:
        return "skip";
    case MoviePolicy::blank:
        return "blank";
    case MoviePolicy::hle:
        return "hle";
    case MoviePolicy::stop:
        break;
    }
    return "stop";
}

std::optional<MoviePolicy> resolve_movie_policy(std::string_view option,
                                                const std::optional<std::filesystem::path>& library,
                                                const core::RegionProfile* profile) {
    if (option == "play" || option == "nodecode") {
        return MoviePolicy::hle;
    }
    if (option == "auto") {
        if (profile != nullptr && movie_audio_problem(*profile)) {
            return MoviePolicy::skip;
        }
        const rt::MovieLibraryConfig config{library ? library->string() : std::string()};
        return rt::movie_library_problem(config) ? MoviePolicy::skip : MoviePolicy::hle;
    }
    return parse_movie_policy(option);
}

std::optional<std::string> movie_audio_problem(const core::RegionProfile& profile) {
    for (const core::RegionProfile::IopModule& module : profile.iop.modules) {
        for (const std::string& service : module.services) {
            if (service.rfind("null:", 0) == 0) {
                return "the IOP module " + module.name + " is still answered by a null server (" + service +
                       "), so a movie's audio would never drain; --movies auto skips until the sound modules "
                       "are "
                       "interpreted (M4 bring-up B8), --movies play or nodecode plays anyway";
            }
        }
    }
    return std::nullopt;
}

std::vector<rt::OverrideSpec> movie_overrides(const core::RegionProfile& profile, MoviePolicy policy) {
    std::vector<rt::OverrideSpec> out;
    if (policy == MoviePolicy::hle) {
        return out;
    }
    for (std::size_t i = 0; i < profile.movie.player_entries.size(); ++i) {
        const std::uint32_t entry = profile.movie.player_entries[i];
        rt::OverrideSpec spec;
        spec.unit = std::string(main_unit_name);
        spec.address = entry;
        spec.kind = rt::OverrideKind::replace;
        spec.name = "movie-" + std::string(to_string(policy)) + "-" + std::to_string(i);
        if (policy == MoviePolicy::stop) {
            spec.fn = [entry](rt::OverrideCall& call) {
                rt::StopInfo s;
                s.reason = rt::StopReason::unsupported;
                s.pc = call.ctx->pc;
                s.address = entry;
                s.message = "--movies stop: the game called the movie player entry " + rt::hex32(entry) +
                            " (a0=" + rt::hex32(rt::gpr_u32(*call.ctx, 4)) +
                            "); movie playback is M4, --movies skip or blank returns from it instead";
                machine_of(call).stop(std::move(s));
            };
        } else {
            spec.fn = [entry](rt::OverrideCall& call) {
                rt::set_gpr_lo(*call.ctx, reg_v0,
                               static_cast<std::uint64_t>(std::int64_t{movie_skip_entry_result}));
                machine_of(call).emit(rt::MachineEvent{"movie_skip", {}}
                                          .add("entry", rt::hex32(entry))
                                          .add("a0", rt::hex32(rt::gpr_u32(*call.ctx, 4))));
                call.return_to_caller();
            };
        }
        out.push_back(std::move(spec));
    }
    if (policy != MoviePolicy::stop && profile.movie.finished_query != 0) {
        rt::OverrideSpec spec;
        spec.unit = std::string(main_unit_name);
        spec.address = profile.movie.finished_query;
        spec.kind = rt::OverrideKind::replace;
        spec.name = "movie-" + std::string(to_string(policy)) + "-finished";
        spec.fn = [](rt::OverrideCall& call) {
            rt::set_gpr_lo(*call.ctx, reg_v0, 1); // the movie is done
            call.return_to_caller();
        };
        out.push_back(std::move(spec));
    }
    return out;
}

std::vector<rt::OverrideSpec> movie_hle_overrides(const core::RegionProfile& profile, rt::SceMpegHle& hle) {
    std::vector<rt::OverrideSpec> out;
    const core::RegionProfile::Movie::SceMpeg& m = profile.movie.sce_mpeg;
    if (!m.present()) {
        return out;
    }
    rt::SceMpegHle* h = &hle;
    const auto add = [&out](std::uint32_t address, rt::OverrideKind kind, const char* name,
                            rt::OverrideFn fn) {
        rt::OverrideSpec spec;
        spec.unit = std::string(main_unit_name);
        spec.address = address;
        spec.kind = kind;
        spec.name = std::string("sce-mpeg-") + name;
        spec.fn = std::move(fn);
        out.push_back(std::move(spec));
    };
    using K = rt::OverrideKind;
    add(m.create, K::hook_before, "create", [h](rt::OverrideCall& c) { h->hook_create(c); });
    add(m.delete_, K::hook_before, "delete", [h](rt::OverrideCall& c) { h->hook_delete(c); });
    add(m.add_callback, K::hook_before, "add-callback",
        [h](rt::OverrideCall& c) { h->hook_add_callback(c); });
    add(m.add_str_callback, K::hook_before, "add-str-callback",
        [h](rt::OverrideCall& c) { h->hook_add_str_callback(c); });
    add(m.demux_pss_ring, K::hook_before, "demux-pss-ring",
        [h](rt::OverrideCall& c) { h->hook_demux_pss_ring(c); });
    add(m.reset, K::hook_before, "reset", [h](rt::OverrideCall& c) { h->hook_reset(c); });
    add(m.get_picture, K::replace, "get-picture", [h](rt::OverrideCall& c) { h->get_picture(c); });
    add(m.get_picture_inner, K::replace, "ipu-decode", [h](rt::OverrideCall& c) { h->decode_reached(c); });
    add(m.is_end, K::replace, "is-end", [h](rt::OverrideCall& c) { h->is_end(c); });
    add(m.video_stream_callback, K::replace, "video-stream",
        [h](rt::OverrideCall& c) { h->video_stream(c); });
    if (m.player_open != 0) {
        const unsigned name_arg = m.player_open_name_arg;
        add(m.player_open, K::hook_before, "player-open",
            [h, name_arg](rt::OverrideCall& c) { h->hook_player_open(c, name_arg); });
    }
    return out;
}

} // namespace dq8::game
