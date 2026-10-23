// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/settings.h"
#include "host/options.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Settings to dq8-game (Docs/milestones/M5-launcher.md, "Settings to dq8-game"; ADR-0006). With --config or
// --set, run_game loads a settings store and turns it into Options before anything reads them: each row of
// the table below sets its Options fields unless its flag is on the command line (flags win over --set, which
// wins over --config) or its schema conditions ("requires") do not hold. Without either option no setting
// applies and the run is exactly M4's. Each M5 session adds the rows of the settings it wires up.
namespace dq8::host {

// The schema shipped with the program (found from `executable_dir`), --config in Layer::user and --set in
// Layer::overrides; nullopt without --config and --set. A missing --config file leaves the defaults. Throws
// core::Error: settings_parse for a damaged file, settings_invalid for a bad --set, io_open without a schema.
std::optional<core::settings::Store> load_settings(const Options& o, const fs::path& executable_dir);

// One setting and how it reaches the Options.
struct SettingOption {
    std::string_view key;  // "audio.output"
    std::string_view flag; // "audio": when it is on the command line, the flag wins
    // Sets the Options fields; false when the setting does not apply to this run (e.g. the internal
    // resolution without --gs parallel, or next to an explicit --ssaa).
    bool (*apply)(Options& o, const core::settings::Store& store);
};

// Every setting dq8-game applies, in schema order.
const std::vector<SettingOption>& setting_options();

// Applies the rows to `o` (choose the store's profile first: Store::use_profile). Returns what was applied,
// as "key=value" (for the --json report and tests).
std::vector<std::string> apply_settings(Options& o, const core::settings::Store& store);

} // namespace dq8::host
