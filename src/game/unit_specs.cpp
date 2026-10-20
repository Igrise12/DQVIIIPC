// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "game/unit_specs.h"

#include <algorithm>
#include <cctype>

namespace dq8::game {

rt::OverlayWindowSpec overlay_window_spec(const core::RegionProfile& profile) {
    return {profile.overlay_window.base, profile.overlay_window.end};
}

std::string overlay_unit_name(const core::ProfileOverlay& overlay) {
    std::string name = overlay.name;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return name;
}

std::vector<rt::UnitSpec> unit_specs(const core::RegionProfile& profile) {
    std::vector<rt::UnitSpec> specs;
    rt::UnitSpec main;
    main.name = std::string(main_unit_name);
    main.kind = DQ8_UNIT_MAIN;
    main.overlay_id = 0;
    main.profile_id = profile.id;
    main.source_sha1 = profile.boot.sha1;
    main.entry_point = profile.boot.entry;
    main.range_begin = profile.boot.load_address;
    main.range_end = profile.boot.load_address + profile.boot.main_memsz;
    specs.push_back(main);

    std::vector<const core::ProfileOverlay*> overlays;
    for (const core::ProfileOverlay& o : profile.overlays) {
        overlays.push_back(&o);
    }
    std::sort(
        overlays.begin(), overlays.end(),
        [](const core::ProfileOverlay* a, const core::ProfileOverlay* b) { return a->index < b->index; });
    const auto& window = profile.overlay_window;
    for (const core::ProfileOverlay* o : overlays) {
        rt::UnitSpec spec;
        spec.name = overlay_unit_name(*o);
        spec.kind = DQ8_UNIT_OVERLAY;
        spec.overlay_id = o->id;
        spec.profile_id = profile.id;
        spec.source_sha1 = o->sha1;
        spec.entry_point = 0;
        spec.range_begin = window.base;
        spec.range_end = window.end;
        if (o->segment_memsz != 0 && o->segment_memsz < window.end - window.base) {
            spec.range_end = window.base + o->segment_memsz;
        }
        specs.push_back(spec);
    }
    return specs;
}

} // namespace dq8::game
