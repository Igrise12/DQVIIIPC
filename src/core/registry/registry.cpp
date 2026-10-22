// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The build registry (dq8/core/registry.h). load() and save() are the M5 day-0 stubs that M5 session A
// replaces.
#include "dq8/core/registry.h"

#include "platform/paths.h"

#include <stdexcept>
#include <utility>

namespace dq8::core::registry {

Registry Registry::load(const std::filesystem::path& /*file*/) {
    throw std::logic_error("registry: load is not implemented (M5 session A)");
}

void Registry::save(const std::filesystem::path& /*file*/) const {
    throw std::logic_error("registry: save is not implemented (M5 session A)");
}

const Install* Registry::find(std::string_view profile_id) const noexcept {
    for (const Install& install : installs_) {
        if (install.profile_id == profile_id) {
            return &install;
        }
    }
    return nullptr;
}

void Registry::upsert(Install install) {
    for (Install& existing : installs_) {
        if (existing.profile_id == install.profile_id) {
            existing = std::move(install);
            return;
        }
    }
    installs_.push_back(std::move(install));
}

std::filesystem::path default_file() {
    return platform::user_data_dir() / "installs.toml";
}

} // namespace dq8::core::registry
