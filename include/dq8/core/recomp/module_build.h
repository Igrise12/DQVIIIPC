// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/process.h"
#include "dq8/core/recomp/units.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Stage 6 of the setup pipeline: build one code unit's generated C++ into a game-module shared library
// through the installed module template (cmake/module: a standalone CMake project, one build tree per unit,
// parameters as cache variables). Configure and build are plain `cmake` invocations through run_process();
// the build tree is incremental, so a re-run after an unchanged generate is a no-op.
namespace dq8::core::recomp {

// What the module template needs to know about the unit (its DQ8_UNIT_* cache variables).
struct ModuleUnit {
    std::string name;                    // DQ8_UNIT_NAME; the library is dq8_unit_<name>
    UnitKind kind = UnitKind::main;      // DQ8_UNIT_KIND: main | overlay
    std::uint32_t overlay_id = 0;        // DQ8_UNIT_OVERLAY_ID
    std::string profile_id;              // DQ8_UNIT_PROFILE_ID
    std::string source_sha1;             // DQ8_UNIT_SOURCE_SHA1: the boot file's or the overlay file's
    std::uint32_t entry_point = 0;       // DQ8_UNIT_ENTRY_POINT
    std::filesystem::path generated_dir; // DQ8_UNIT_GENERATED_DIR
};

struct ModuleBuildOptions {
    std::filesystem::path cmake = "cmake";
    std::string cmake_generator = "Ninja";
    std::filesystem::path module_source_dir; // the template: <prefix>/cmake/module
    std::filesystem::path abi_include_dir;   // DQ8_ABI_INCLUDE_DIR: the directory holding dq8/abi/
    std::filesystem::path build_dir;         // one per unit
    std::string generator_id;                // DQ8_GENERATOR: identifies the recompiler build
    std::string build_type = "Release";
    bool pch = true;                               // DQ8_MODULE_PCH
    bool debug_info = false;                       // DQ8_MODULE_DEBUG_INFO
    unsigned jobs = 1;                             // cmake --build --parallel
    std::vector<std::string> extra_configure_args; // e.g. -DCMAKE_CXX_COMPILER=...
    std::filesystem::path log_file;                // both steps' output; empty: none
    std::chrono::milliseconds timeout{0};          // per step
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(OutputStream, std::string_view)> on_output;
};

struct ModuleBuildResult {
    std::filesystem::path library; // inside the build tree
    std::chrono::milliseconds configure_time{0};
    std::chrono::milliseconds build_time{0};
    unsigned jobs = 0;
};

// The job count for a unit build: min(threads, (available - 3 GiB) / 0.4 GiB), at least 1; `threads` alone
// when the available memory is unknown. 0.4 GiB per job is spike S1's figure for a typical generated file
// with the precompiled header; 3 GiB is headroom for the system and the two largest files.
unsigned default_build_jobs(unsigned threads, std::optional<std::uint64_t> available_bytes) noexcept;
// The same from this host's facts.
unsigned default_build_jobs();

// `cmake -S <template> -B <build> -G <generator> -D...` for the unit.
std::vector<std::string> module_configure_args(const ModuleBuildOptions& options, const ModuleUnit& unit);
// True when <build_dir>/CMakeCache.txt was written for another template than `source_dir` (its
// CMAKE_HOME_DIRECTORY names another directory, or one that no longer exists). cmake refuses to configure
// such a tree, so build_module() configures it afresh (`cmake --fresh`). The template moves when the program
// does: an AppImage is mounted at a new path on every launch, an update or reinstall changes the prefix.
bool configured_for_other_template(const std::filesystem::path& build_dir,
                                   const std::filesystem::path& source_dir);
// `cmake --build <build> --config <type> --parallel <jobs>`.
std::vector<std::string> module_build_args(const ModuleBuildOptions& options);
// The library's file name on this platform, first the one cmake/module produces ("dq8_unit_main.so",
// "dq8_unit_main.dll"), then fallbacks. build_module() looks in <build>/module/, then the whole build tree.
std::vector<std::string> module_file_names(std::string_view unit);

// Configures and builds. Throws Error(process_spawn) when cmake cannot start, process_timeout /
// process_cancelled, module_build_failed for a failed step (with the last lines of output and the log path)
// or when no library turns up in the build tree, tool_missing when the template or the ABI headers are
// missing.
ModuleBuildResult build_module(const ModuleBuildOptions& options, const ModuleUnit& unit);

} // namespace dq8::core::recomp
