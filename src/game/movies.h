// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/region_profile.h"
#include "runtime/ee/overrides.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// Movies in M3 (playback is M4): overrides on the profile's [movie] player entry points.
//   stop  every player entry stops the guest with a diagnostic naming the entry (bring-up uses it to find
//         out which scene plays what, and when).
//   skip  the player entries (open, start) return at once, and the profile's finished_query reports the movie
//         done, so the scene's "update until finished" loop ends at once (M3 bring-up found the scene ignores
//         the entries' results and spins on finished_query). Every skipped entry emits `movie_skip`.
// The scene functions in [movie] are the fallback targets bring-up may move the skip to.
namespace dq8::game {

enum class MoviePolicy { skip, stop };

std::optional<MoviePolicy> parse_movie_policy(std::string_view text) noexcept;
std::string_view to_string(MoviePolicy policy) noexcept;

// The $v0 a skipped player entry returns (the scenes do not read it).
inline constexpr std::int32_t movie_skip_entry_result = 0;

// One override per [movie] player entry, plus finished_query in skip mode (unit "main"); empty when the
// profile has no [movie] table.
std::vector<rt::OverrideSpec> movie_overrides(const core::RegionProfile& profile, MoviePolicy policy);

} // namespace dq8::game
