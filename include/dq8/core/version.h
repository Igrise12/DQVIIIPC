// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

namespace dq8::core {

// Product version string, e.g. "0.0.1". The module ABI version is tracked separately (architecture.md §13.3).
const char* version() noexcept;

} // namespace dq8::core
