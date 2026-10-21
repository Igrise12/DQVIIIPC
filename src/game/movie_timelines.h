// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/region_profile.h"
#include "runtime/machine/machine_services.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// --movies skip for the scene timelines that run alongside a movie ([[movie.timelines]], ADR-0013, proposed).
// The movie overrides end a movie at once, but a scene that steps its own frame counter while the movie plays
// keeps stepping it, on black (M3 bring-up: TITLE's flyover). At every vblank start, in the last phase, this
// device checks each timeline: the overlay id in the window header must be the timeline's overlay, the stage
// variable must hold stage_value, and the counter, a signed 32-bit word, must be above 0 and below skip_to.
// Above 0 means the scene has stepped it: a stage starts its timeline at 0, and a stage variable can hold
// stage_value before the scene runs (SLUS-21207's does from power-on). Then the device writes skip_to to the
// counter, the step the game itself jumps to when the player skips the movie, and emits `movie_timeline_skip`
// {timeline, from, to}. The device keeps no state of its own: whether it writes is a function of guest memory
// at that vblank, and a timeline that starts again is skipped again.
namespace dq8::game {

class MovieTimelineSkip final : public rt::IDevice, private rt::IVblankListener {
  public:
    // Throws std::invalid_argument when the profile has no [[movie.timelines]] rows.
    explicit MovieTimelineSkip(const core::RegionProfile& profile);

    std::string_view name() const override { return "game.movie_timeline_skip"; }
    void attach(rt::MachineServices& machine) override;
    void reset() override;

    std::uint64_t skips() const noexcept { return skips_; }

  private:
    struct Timeline {
        std::string name;
        std::uint32_t overlay_id = 0;
        std::uint32_t stage_variable = 0;
        std::uint32_t stage_width = 0;
        std::uint32_t stage_value = 0;
        std::uint32_t counter = 0;
        std::int32_t skip_to = 0;
    };

    void on_vblank_start(std::uint64_t field) override;

    std::uint32_t id_address_; // window base + 4: the resident overlay's id in its header
    std::vector<Timeline> timelines_;
    rt::MachineServices* machine_ = nullptr;
    std::uint64_t skips_ = 0;
};

} // namespace dq8::game
