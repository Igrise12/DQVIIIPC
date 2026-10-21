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
//   skip  every player entry returns at once with the value the scene code takes as "movie finished".
//         PLACEHOLDER: that value is an M3 bring-up unknown ("movie-skip return values" in
//         Docs/milestones/M3-boots.md); until it is resolved `skip` returns movie_skip_placeholder_result
//         and emits `movie_skip` with placeholder=true so a run that depends on it is visible as such.
// The scene functions in [movie] are the fallback targets bring-up may move the skip to.
namespace dq8::game {

enum class MoviePolicy { skip, stop };

std::optional<MoviePolicy> parse_movie_policy(std::string_view text) noexcept;
std::string_view to_string(MoviePolicy policy) noexcept;

// PLACEHOLDER (M3 bring-up): the $v0 a skipped player entry returns.
inline constexpr std::int32_t movie_skip_placeholder_result = 0;

// One override per [movie] player entry (unit "main"); empty when the profile has no [movie] table.
std::vector<rt::OverrideSpec> movie_overrides(const core::RegionProfile& profile, MoviePolicy policy);

} // namespace dq8::game
