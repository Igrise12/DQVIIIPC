// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/region_profile.h"
#include "runtime/iop/iop.h"

#include <string>
#include <vector>

// The region profile's [iop] table as the IOP's module specs (ADR-0011): which modules the game loads and the
// native services that stand in for each. The runtime's catalogue validates the service names when the IOP
// is built; this only carries them over.
namespace dq8::game {

// One spec per [[iop.modules]] row, in the profile's order (the game's load order).
std::vector<rt::IopModuleSpec> iop_module_specs(const core::RegionProfile& profile);

// The disc path of the IOP update image the game resets the IOP with ("" when the profile has none).
std::string iop_reset_image(const core::RegionProfile& profile);

} // namespace dq8::game
