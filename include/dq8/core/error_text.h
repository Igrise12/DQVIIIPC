// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/error.h"

#include <string_view>

// What the launcher and dq8ctl tell a person about an error (architecture.md §11.4): a short message and the
// action that fixes it, for every ErrorCode. The code's stable name (to_string) identifies it in logs and
// diagnostics; these texts may be reworded, the names may not.
namespace dq8::core {

struct ErrorText {
    std::string_view message; // one sentence, what happened
    std::string_view action;  // one sentence, what to do
};

ErrorText error_text(ErrorCode code) noexcept;

} // namespace dq8::core
