// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "game/iop_modules.h"

namespace dq8::game {

std::vector<rt::IopModuleSpec> iop_module_specs(const core::RegionProfile& profile) {
    std::vector<rt::IopModuleSpec> specs;
    specs.reserve(profile.iop.modules.size());
    for (const core::RegionProfile::IopModule& m : profile.iop.modules) {
        specs.push_back({m.name, m.services});
    }
    return specs;
}

std::string iop_reset_image(const core::RegionProfile& profile) {
    return profile.iop.reset_image;
}

} // namespace dq8::game
