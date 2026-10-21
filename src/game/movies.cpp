// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "game/movies.h"

#include "game/unit_specs.h"
#include "runtime/ee/context.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/machine/machine_services.h"

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
    case MoviePolicy::stop:
        break;
    }
    return "stop";
}

std::vector<rt::OverrideSpec> movie_overrides(const core::RegionProfile& profile, MoviePolicy policy) {
    std::vector<rt::OverrideSpec> out;
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

} // namespace dq8::game
