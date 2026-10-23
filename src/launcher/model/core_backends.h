// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "model/diagnostics.h"
#include "model/wizard.h"

#include <filesystem>
#include <memory>
#include <optional>

// The wizard over libdq8core: verify_image() against the region profiles found next to the program,
// toolchain::detect() and check(), setup::run().
namespace dq8::launcher {

// data/regions under an ancestor of `executable_dir` or share/dq8/regions beside it, else data/regions under
// the current directory (dq8-game and dq8ctl look in the same places, the current directory first; a launcher
// is rarely started from a source tree, and its own files should win).
std::optional<std::filesystem::path> find_regions_dir(const std::filesystem::path& executable_dir);

std::unique_ptr<WizardBackend> core_wizard_backend(std::filesystem::path executable_dir);

// The Diagnostics page's export through dq8/core/diagnostics.h, in process (the report dq8ctl diagnose
// writes): the settings and registry found from `executable_dir`, never game data.
std::unique_ptr<DiagnosticsExporter> core_diagnostics(std::filesystem::path executable_dir);

} // namespace dq8::launcher
