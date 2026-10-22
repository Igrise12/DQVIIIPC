// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/error.h"

#include <filesystem>
#include <string>
#include <vector>

// The tools the setup needs on the user's machine (ADR-0008, ADR-0018): a C and C++ compiler, cmake and
// ninja. On Linux the system compiler with cmake and ninja shipped next to the program or on PATH; on Windows
// the pinned llvm-mingw bundle in the user data directory (downloaded with the user's consent; M5 session E).
// M5 day 0: declarations; M5 session A implements detect() and check(), session E the Windows bundle.
namespace dq8::core::toolchain {

struct Tool {
    bool found = false;
    std::filesystem::path path;
    std::string version; // as the tool reports it, e.g. "13.2.0", "3.28.3"
};

struct Toolchain {
    Tool cc;
    Tool cxx;
    Tool cmake;
    Tool ninja;
    std::string compiler_id; // "gnu", "clang", "llvm-mingw"
    bool bundled = false;    // the tools come from the bundled toolchain
    std::filesystem::path bundled_root;
};

struct DetectOptions {
    std::filesystem::path
        executable_dir;                 // cmake and ninja shipped with the program are looked for here first
    std::filesystem::path bundled_root; // empty: platform::user_data_dir() / "toolchain"
    bool use_path = true;               // also look on PATH
};

Toolchain detect(const DetectOptions& options);

struct Problem {
    ErrorCode code = ErrorCode::toolchain_incomplete;
    std::string tool;   // "c++ compiler", "cmake", "ninja"
    std::string advice; // what to install, for this platform (and on Linux the distribution family)
};

// Empty when the toolchain can build the game modules (versions included: cmake >= 3.24, a C++20 compiler).
std::vector<Problem> check(const Toolchain& toolchain);

// Parallel compile jobs for this machine: recomp::default_build_jobs() (threads, bounded by free memory).
unsigned default_jobs();

} // namespace dq8::core::toolchain
