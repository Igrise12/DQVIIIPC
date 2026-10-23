// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "model/errors.h"

#include "dq8/core/error_text.h"

namespace dq8::launcher {

std::optional<core::ErrorCode> error_code_from_name(std::string_view name) noexcept {
    if (name.empty()) {
        return std::nullopt;
    }
    // The codes are numbered from 0 without gaps and only ever appended; to_string() says "unknown" past the
    // last. (ErrorCode is an enum class over int, so every value in the loop is a valid ErrorCode.)
    for (int i = 0; i < 4096; ++i) {
        const auto code = static_cast<core::ErrorCode>(i);
        const std::string_view spelled = core::to_string(code);
        if (spelled == "unknown") {
            break;
        }
        if (spelled == name) {
            return code;
        }
    }
    return std::nullopt;
}

Problem explain(core::ErrorCode code, std::string_view detail) {
    const core::ErrorText text = core::error_text(code);
    Problem p;
    p.code = code;
    p.code_name = std::string(core::to_string(code));
    p.message = std::string(text.message);
    p.action = std::string(text.action);
    p.detail = std::string(detail);
    return p;
}

Problem explain_name(std::string_view code_name, std::string_view fallback_message) {
    if (const auto code = error_code_from_name(code_name)) {
        return explain(*code, fallback_message);
    }
    Problem p;
    p.code_name = std::string(code_name);
    p.message = std::string(fallback_message);
    return p;
}

Problem explain_exception(const std::exception& e) {
    if (const auto* error = dynamic_cast<const core::Error*>(&e)) {
        return explain(error->code(), error->what());
    }
    Problem p;
    p.message = e.what();
    return p;
}

Problem explain_current_exception() {
    try {
        throw;
    } catch (const std::exception& e) {
        return explain_exception(e);
    } catch (...) {
        Problem p;
        p.message = "an unknown error";
        return p;
    }
}

} // namespace dq8::launcher
