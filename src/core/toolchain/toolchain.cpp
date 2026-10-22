// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// M5 day-0 stub of the toolchain API (dq8/core/toolchain.h); M5 session A implements detect() and check().
#include "dq8/core/toolchain.h"

#include "dq8/core/recomp/module_build.h"

#include <stdexcept>

namespace dq8::core::toolchain {

Toolchain detect(const DetectOptions& /*options*/) {
    throw std::logic_error("toolchain: detect is not implemented (M5 session A)");
}

std::vector<Problem> check(const Toolchain& /*toolchain*/) {
    throw std::logic_error("toolchain: check is not implemented (M5 session A)");
}

unsigned default_jobs() {
    return recomp::default_build_jobs();
}

} // namespace dq8::core::toolchain
