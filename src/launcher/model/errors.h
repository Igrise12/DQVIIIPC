// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/error.h"

#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace dq8::launcher {

// The ErrorCode whose stable name (core::to_string) is `name`, e.g. "smoke-failed".
std::optional<core::ErrorCode> error_code_from_name(std::string_view name) noexcept;

// What a page shows for a failure: the code's text from error_text() when the code is known, the detail the
// component reported, and the code's stable name for reports.
struct Problem {
    std::optional<core::ErrorCode> code;
    std::string code_name; // the stable name, or a session-defined kebab name, or empty
    std::string message;   // what happened (error_text's message, else the reported text)
    std::string action;    // what to do (error_text's action), may be empty
    std::string detail;    // the component's own message (file names, values), may be empty
};

Problem explain(core::ErrorCode code, std::string_view detail = {});
// A code by name: known names get error_text(); others keep `fallback_message` as the message.
Problem explain_name(std::string_view code_name, std::string_view fallback_message);
// A caught exception: core::Error gets its code; anything else (the day-0 stubs throw std::logic_error) is
// shown with its what() and no code.
Problem explain_exception(const std::exception& e);
Problem explain_current_exception(); // inside a catch block

} // namespace dq8::launcher
