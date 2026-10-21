// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "host/options.h"

#include <stdexcept>

// One run of dq8-game after option parsing (run_machine.cpp).
namespace dq8::host {

// The GS backend or the platform failed to start: exit_backend.
class BackendError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// Returns the exit status. Throws UsageError, SetupProblems, BackendError, core::Error.
int run_game(const Options& o, const char* argv0);

} // namespace dq8::host
