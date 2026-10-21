// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/region_profile.h"
#include "runtime/io/input_script.h"
#include "runtime/machine/machine_services.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// dq8-game --until: the game state that ends a run successfully, checked at the end of every one-vblank
// slice (so the run always ends on a vblank boundary and stays deterministic).
//   vblank:N      N vblanks have started
//   overlay:NAME  the residency hook made overlay NAME resident (a profile overlay name, any case)
//   mode:N        the mode watcher saw the game mode become N
//   title         the title screen; needs the profile's [title] marker, which M3 bring-up adds
// Machine events become the same triggers the input scripts wait for (trigger_from_event).
namespace dq8::game {

struct UntilCondition {
    enum class Kind { vblank, trigger, title };
    Kind kind = Kind::vblank;
    std::uint64_t vblank = 0; // Kind::vblank
    rt::InputTrigger trigger; // Kind::trigger ("overlay" or "mode")
    std::string text;         // as given, for messages
};

// Throws std::invalid_argument with a usage message. `profile` (null in developer mode) validates overlay
// names and supplies the title marker.
UntilCondition parse_until(std::string_view text, const core::RegionProfile* profile);

// The input-script trigger a machine event stands for: overlay_resident -> overlay:NAME, mode -> mode:N,
// rpc_bind -> rpc_bind:SID; nullopt for every other event.
std::optional<rt::InputTrigger> trigger_from_event(const rt::MachineEvent& event);

class UntilTracker {
  public:
    explicit UntilTracker(UntilCondition condition) : condition_(std::move(condition)) {}

    // Every machine event, with MachineServices::vblank_count() when it was emitted.
    void observe(const rt::MachineEvent& event, std::uint64_t vblank);
    // At the end of each slice.
    void at_vblank(std::uint64_t vblank);

    bool met() const noexcept { return met_at_.has_value(); }
    // The vblank count at which the condition was first seen to hold.
    std::optional<std::uint64_t> met_at() const noexcept { return met_at_; }
    const UntilCondition& condition() const noexcept { return condition_; }

  private:
    UntilCondition condition_;
    std::optional<std::uint64_t> met_at_;
};

} // namespace dq8::game
