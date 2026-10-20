// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/region_profile.h"
#include "runtime/ee/game_module.h"
#include "runtime/ee/overlay_manager.h"

#include <string>
#include <string_view>
#include <vector>

// The game layer's answer to "what code units does this disc have, and where do they live?". Everything comes
// from the region profile (data/regions/<id>.toml); the runtime only sees the resulting specs.
namespace dq8::game {

// The main program's unit name; overlay units are named after their profile label in lower case.
inline constexpr std::string_view main_unit_name = "main";

// The overlay window the profile records.
rt::OverlayWindowSpec overlay_window_spec(const core::RegionProfile& profile);

// "BATTLE" -> "battle": the unit name (and module file name part) of an overlay.
std::string overlay_unit_name(const core::ProfileOverlay& overlay);

// One spec per code unit: the main program first, then every overlay in loader-index order (the profile's
// name_table_order). The main unit's table must lie in its first load segment
// [boot.load_address, boot.load_address + boot.main_memsz) and start at boot.entry; an overlay's must lie in
// the window, within the overlay's own segment when its memsz is recorded. Source hashes are the boot file's
// and each overlay container's; an unrecorded hash is not checked.
std::vector<rt::UnitSpec> unit_specs(const core::RegionProfile& profile);

} // namespace dq8::game
