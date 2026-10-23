// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/error.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The tools the setup needs on the user's machine (ADR-0008, ADR-0018): a C++20 compiler, cmake and ninja
// (the module template, cmake/module, is C++ only). On Linux the system compiler with cmake and ninja shipped
// next to the program or on PATH; on Windows the pinned llvm-mingw bundle in the user data directory
// (downloaded with the user's consent; M5 session E, through DetectOptions::bundled_root).
namespace dq8::core::toolchain {

struct Tool {
    bool found = false;
    std::filesystem::path path;
    std::string version; // as the tool reports it, e.g. "13.2.0", "3.28.3"; empty when it said none
};

// The operating system, for install advice and diagnostics.
struct Distribution {
    std::string id;          // os-release ID ("ubuntu", "fedora", "cachyos"), "windows", "macos"
    std::string family;      // "debian", "fedora", "arch", "suse", "alpine", "gentoo", "void", "nixos",
                             // "windows", "macos"; empty for an unknown Linux distribution
    std::string pretty_name; // os-release PRETTY_NAME, or the platform's name
};

// The family of an /etc/os-release text: ID first, then each ID_LIKE word.
Distribution parse_os_release(std::string_view text);
// This machine: /etc/os-release (else /usr/lib/os-release) on Linux.
Distribution this_distribution();

struct Toolchain {
    Tool cc;
    Tool cxx;
    Tool cmake;
    Tool ninja;
    std::string compiler_id; // the C++ compiler: "gnu", "clang", "llvm-mingw"; empty when unknown
    bool bundled = false;    // the compiler comes from the bundled toolchain
    std::filesystem::path bundled_root;
    std::filesystem::path bundle_dir; // the installed bundle (<bundled_root>/<version>) when bundled
    Distribution distribution;
};

struct DetectOptions {
    std::filesystem::path
        executable_dir;                 // cmake and ninja shipped with the program are looked for here first
    std::filesystem::path bundled_root; // empty: platform::user_data_dir() / "toolchain"; the bundle itself
                                        // is installed_toolchain(bundled_root) (session E's download.h)
    bool use_path = true;               // also look on PATH
    bool use_environment = true;        // CC and CXX name the compilers when set (as cmake reads them)
};

// Finds the tools and asks each for its version (`--version`, ten seconds at most). Never throws for a tool
// that is missing or does not answer; check() reports it.
Toolchain detect(const DetectOptions& options);

struct Problem {
    ErrorCode code = ErrorCode::toolchain_incomplete;
    std::string tool;   // "c++ compiler", "cmake", "ninja"
    std::string advice; // what is wrong and what to install, for this platform and distribution family
};

// Empty when the toolchain can build the game modules: a C++ compiler (GCC 11 or Clang 14 or newer when the
// version is known), cmake 3.25 or newer (cmake/module's minimum) and ninja.
std::vector<Problem> check(const Toolchain& toolchain);

// The command that installs `tool` ("c++ compiler", "cmake", "ninja") on this distribution, as advice text.
std::string install_advice(std::string_view tool, const Distribution& distribution);

// The first "N.N" or "N.N.N" in a tool's --version text ("g++ (GCC) 13.2.0" -> "13.2.0"); empty when none.
std::string parse_version(std::string_view text);
// Compares dotted versions numerically ("3.9" < "3.25"): negative, 0 or positive.
int compare_versions(std::string_view a, std::string_view b);

// The cache variables that make cmake/module use exactly these tools: CMAKE_CXX_COMPILER and
// CMAKE_MAKE_PROGRAM, for each tool that was found.
std::vector<std::string> cmake_arguments(const Toolchain& toolchain);

// Parallel compile jobs for this machine: recomp::default_build_jobs() (threads, bounded by free memory).
unsigned default_jobs();

} // namespace dq8::core::toolchain
