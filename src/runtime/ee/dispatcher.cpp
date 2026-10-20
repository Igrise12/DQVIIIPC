// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/dispatcher.h"

#include "runtime/ee/guest_stop.h"

#include <utility>

namespace dq8::rt {

void Dispatcher::set_main(std::string name, DispatchTable table) {
    main_name_ = std::move(name);
    main_ = table;
}

DispatchTarget Dispatcher::resolve(std::uint32_t addr) const noexcept {
    if (const dq8_guest_fn fn = main_.lookup(addr)) {
        return {fn, main_name_};
    }
    if (overlays_.in_window(addr)) {
        const OverlayUnit* res = overlays_.resident();
        if (res != nullptr) {
            if (const dq8_guest_fn fn = res->table.lookup(addr)) {
                return {fn, res->name};
            }
        }
    }
    return {};
}

std::string Dispatcher::describe_miss(std::uint32_t addr) const {
    const std::string at = "no recompiled function at " + hex32(addr) + ": ";
    if ((addr & 3u) != 0) {
        return at + "the address is not 4-aligned";
    }
    if (main_.covers(addr)) {
        return at + "the " + main_name_ + " unit's table [" + hex32(main_.base()) + ", " +
               hex32(main_.end()) +
               ") has no entry there (not a function start in the map, nor a resume point)";
    }
    if (overlays_.in_window(addr)) {
        return at + overlays_.describe_miss(addr);
    }
    std::string out = at + "outside every dispatch table";
    if (!main_.empty()) {
        out += " (" + main_name_ + " [" + hex32(main_.base()) + ", " + hex32(main_.end()) + ")";
        if (const auto& w = overlays_.window()) {
            out += ", overlay window [" + hex32(w->base) + ", " + hex32(w->end) + ")";
        }
        out += ")";
    }
    return out;
}

} // namespace dq8::rt
