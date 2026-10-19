// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/version.h"

#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <string>

TEST_CASE("core version string is a dotted number", "[core]") {
    const std::string v = dq8::core::version();
    REQUIRE_FALSE(v.empty());
    REQUIRE(std::isdigit(static_cast<unsigned char>(v.front())));
    REQUIRE(v.find('.') != std::string::npos);
}
