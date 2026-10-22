// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/region_profile.h"
#include "runtime/ee/overrides.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dq8::rt {
class SceMpegHle;
}

// Movies: either the sceMpeg HLE plays them (M4, ADR-0015: --movies play, nodecode, and auto when the
// dq8-movie library loads), or overrides on the profile's [movie] player entry points skip them (M3).
//   hle   the sceMpeg HLE at the profile's [movie.sce_mpeg] addresses (movie_hle_overrides); the player
//         entries run as game code and no timeline is skipped. The host builds the HLE with its decoder
//         (src/host/movie_wiring.cpp).
//   stop  every player entry stops the guest with a diagnostic naming the entry (bring-up uses it to find
//         out which scene plays what, and when).
//   blank the player entries (open, start) return at once, and the profile's finished_query reports the movie
//         done, so the scene's "update until finished" loop ends at once (M3 bring-up found the scene ignores
//         the entries' results and spins on finished_query). Every skipped entry emits `movie_skip`. A scene
//         timeline that runs alongside the movie ([[movie.timelines]]) keeps its own clock: black for the
//         movie's length, as on a console that plays it (M3's skip; for timing comparisons with PCSX2).
//   skip  blank, and the timelines jump to their skip point (MovieTimelineSkip, movie_timelines.h).
// The scene functions in [movie] are the fallback targets bring-up may move the skip to.
namespace dq8::game {

enum class MoviePolicy { skip, blank, stop, hle };

// skip, blank and stop; the HLE's names (play, nodecode, auto) are resolve_movie_policy's.
std::optional<MoviePolicy> parse_movie_policy(std::string_view text) noexcept;
std::string_view to_string(MoviePolicy policy) noexcept;

// What a --movies value resolves to: play and nodecode are hle; auto is hle when the dq8-movie library
// (`library`, or the default search when empty) loads and creates a decoder and `profile`, when given, has no
// movie_audio_problem, else skip; the others as parse_movie_policy. nullopt for an unknown value.
std::optional<MoviePolicy> resolve_movie_policy(std::string_view option,
                                                const std::optional<std::filesystem::path>& library,
                                                const core::RegionProfile* profile = nullptr);

// Why --movies auto must not play on this profile yet, or nullopt. A movie's audio drains only through the
// game's sound modules: while one of the profile's IOP modules is still answered by a null server (M3's
// stand-ins, until bring-up step B8 interprets the sound modules), the first movie with audio waits forever,
// so auto skips. play and nodecode do not ask.
std::optional<std::string> movie_audio_problem(const core::RegionProfile& profile);

// The $v0 a skipped player entry returns (the scenes do not read it).
inline constexpr std::int32_t movie_skip_entry_result = 0;

// One override per [movie] player entry, plus finished_query with skip and blank (unit "main"); empty when
// the profile has no [movie] table, and with hle.
std::vector<rt::OverrideSpec> movie_overrides(const core::RegionProfile& profile, MoviePolicy policy);

// The sceMpeg HLE's overrides at the profile's [movie.sce_mpeg] addresses (unit "main"): hooks before Create,
// Delete, AddCallback, AddStrCallback, DemuxPssRing and Reset; GetPicture, IsEnd and the video stream
// callback replaced; the SDK's inner IPU decode a stop; and, when the table names the game's player open, a
// hook before it that gives the HLE the movie's name. Empty when the profile has no table. `hle` must outlive
// the overrides.
std::vector<rt::OverrideSpec> movie_hle_overrides(const core::RegionProfile& profile, rt::SceMpegHle& hle);

} // namespace dq8::game
