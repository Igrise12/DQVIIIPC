// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "model/diagnostics.h"
#include "model/game_session.h"
#include "model/library.h"
#include "model/settings_model.h"
#include "model/wizard.h"

#include <filesystem>
#include <memory>

// `dq8-launcher --preview`: every page over stand-ins that need no disc, no toolchain and no game, for
// working on the UI and for the smoke tests that draw each page on SDL's dummy driver. The setup and the game
// are simulated on timers; nothing is written. No game data: the "disc" is called PREVIEW.
namespace dq8::launcher::preview {

std::unique_ptr<WizardBackend> wizard_backend();
// `existing_file` and `existing_dir` stand in for the image and the modules so Play is enabled.
std::unique_ptr<LibraryBackend> library_backend(std::filesystem::path existing_file,
                                                std::filesystem::path existing_dir);
// The real settings when core can load them, else a sample of rows shaped like the schema's.
std::unique_ptr<SettingsBackend> settings_backend(const std::filesystem::path& executable_dir);
std::unique_ptr<DiagnosticsExporter> diagnostics_exporter();
// A game that reports the five status lines over a few seconds and runs until stopped.
ProcessRunner game_runner();

} // namespace dq8::launcher::preview
