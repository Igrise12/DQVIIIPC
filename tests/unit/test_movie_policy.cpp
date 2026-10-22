// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// --movies resolution and the sceMpeg HLE's overrides at the profile's [movie.sce_mpeg] addresses
// (src/game/movies.h, ADR-0015).
#include "dq8/core/region_profile.h"
#include "game/movies.h"
#include "runtime/movie/movie_decoder.h"
#include "runtime/movie/sce_mpeg_hle.h"
#include "runtime/platform/dynlib.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

using namespace dq8;

TEST_CASE("movies: --movies values resolve to a policy", "[game][movie]") {
    const std::optional<std::filesystem::path> missing =
        std::filesystem::path("/nonexistent/libdq8-movie.so");
    CHECK(game::resolve_movie_policy("play", missing) == game::MoviePolicy::hle);
    CHECK(game::resolve_movie_policy("nodecode", missing) == game::MoviePolicy::hle);
    CHECK(game::resolve_movie_policy("auto", missing) == game::MoviePolicy::skip); // no library: skip
    CHECK(game::resolve_movie_policy("skip", missing) == game::MoviePolicy::skip);
    CHECK(game::resolve_movie_policy("blank", std::nullopt) == game::MoviePolicy::blank);
    CHECK(game::resolve_movie_policy("stop", std::nullopt) == game::MoviePolicy::stop);
    CHECK_FALSE(game::resolve_movie_policy("fast", std::nullopt));
    // Not a movie decoder (the contract game module): refused, so auto skips.
    const std::filesystem::path contract =
        std::filesystem::path(DQ8_CONTRACT_MODULE_DIR) / rt::shared_library_file_name("dq8_unit_contract");
    CHECK(game::resolve_movie_policy("auto", contract) == game::MoviePolicy::skip);
    CHECK(game::to_string(game::MoviePolicy::hle) == "hle");
    if (!rt::movie_library_problem(rt::MovieLibraryConfig{})) {
        CHECK(game::resolve_movie_policy("auto", std::nullopt) == game::MoviePolicy::hle);
    }
}

TEST_CASE("movies: auto skips while a sound module is still a null server", "[game][movie]") {
    core::RegionProfile profile;
    profile.iop.modules.push_back({"PADMAN", {"padman.v1"}});
    CHECK_FALSE(game::movie_audio_problem(profile));
    profile.iop.modules.push_back({"SDRDRV", {"null:0x80000701"}});
    const auto problem = game::movie_audio_problem(profile);
    REQUIRE(problem);
    CHECK(problem->find("SDRDRV") != std::string::npos);
    // auto skips whether or not a decoder would load; play and nodecode are not asked.
    CHECK(game::resolve_movie_policy("auto", std::nullopt, &profile) == game::MoviePolicy::skip);
    CHECK(game::resolve_movie_policy("play", std::nullopt, &profile) == game::MoviePolicy::hle);
    CHECK(game::resolve_movie_policy("nodecode", std::nullopt, &profile) == game::MoviePolicy::hle);
}

TEST_CASE("movies: the HLE's overrides sit at the profile's sceMpeg addresses", "[game][movie]") {
    core::RegionProfile profile;
    profile.movie.player_entries = {0x00100100u, 0x00100200u};
    profile.movie.finished_query = 0x00100300u;
    CHECK(game::movie_overrides(profile, game::MoviePolicy::hle).empty());
    CHECK(game::movie_overrides(profile, game::MoviePolicy::skip).size() == 3);

    rt::SceMpegHleConfig config;
    config.make_decoder = [] { return rt::make_blank_movie_decoder(); };
    config.video_callback = 0x00100B00u;
    rt::SceMpegHle hle(config);
    CHECK(game::movie_hle_overrides(profile, hle).empty()); // no [movie.sce_mpeg]

    auto& m = profile.movie.sce_mpeg;
    m.create = 0x00100400u;
    m.delete_ = 0x00100500u;
    m.add_callback = 0x00100600u;
    m.add_str_callback = 0x00100700u;
    m.demux_pss_ring = 0x00100800u;
    m.get_picture = 0x00100900u;
    m.get_picture_inner = 0x00100A00u;
    m.is_end = 0x00100A80u;
    m.reset = 0x00100C00u;
    m.ipu_init = 0x00100D00u;
    m.video_stream_callback = 0x00100B00u;
    std::map<std::uint32_t, rt::OverrideSpec> by_address;
    for (rt::OverrideSpec& s : game::movie_hle_overrides(profile, hle)) {
        CHECK(s.unit == "main");
        CHECK(static_cast<bool>(s.fn));
        by_address.emplace(s.address, std::move(s));
    }
    REQUIRE(by_address.size() == 10); // every entry but the IPU init, which runs against the IPU stub
    using K = rt::OverrideKind;
    for (const std::uint32_t hooked :
         {m.create, m.delete_, m.add_callback, m.add_str_callback, m.demux_pss_ring, m.reset}) {
        CHECK(by_address.at(hooked).kind == K::hook_before);
    }
    for (const std::uint32_t replaced :
         {m.get_picture, m.get_picture_inner, m.is_end, m.video_stream_callback}) {
        CHECK(by_address.at(replaced).kind == K::replace);
    }
    CHECK(by_address.at(m.get_picture).name == "sce-mpeg-get-picture");
    CHECK(by_address.count(m.ipu_init) == 0);

    // With [movie.sce_mpeg].player_open, a hook before the player's open as well.
    m.player_open = 0x00100E00u;
    m.player_open_name_arg = 2;
    const auto with_open = game::movie_hle_overrides(profile, hle);
    REQUIRE(with_open.size() == 11);
    CHECK(with_open.back().address == 0x00100E00u);
    CHECK(with_open.back().kind == rt::OverrideKind::hook_before);
    CHECK(with_open.back().name == "sce-mpeg-player-open");
}
