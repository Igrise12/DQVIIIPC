// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/recomp/module_build.h"

#include "dq8/core/error.h"
#include "platform/files.h"
#include "platform/system_info.h"
#include "recomp/common.h"

#include <algorithm>
#include <fstream>

namespace dq8::core::recomp {

namespace fs = std::filesystem;
using detail::hex8;
using detail::path_utf8;

namespace {

constexpr std::uint64_t gib = std::uint64_t{1} << 30;
constexpr std::uint64_t headroom = 3 * gib;
constexpr std::size_t excerpt_lines = 30;

// CMake reads forward slashes on every platform; backslashes in -D values are escape-prone.
std::string cmake_path(const fs::path& p) {
    const std::u8string s = fs::absolute(p).generic_u8string();
    return std::string(s.begin(), s.end());
}

std::string tail_lines(const std::string& text, std::size_t count) {
    std::size_t pos = text.size();
    while (pos > 0 && (text[pos - 1] == '\n' || text[pos - 1] == '\r')) {
        --pos;
    }
    const std::size_t end = pos;
    std::size_t lines = 0;
    while (pos > 0) {
        if (text[pos - 1] == '\n' && ++lines == count) {
            break;
        }
        --pos;
    }
    return text.substr(pos, end - pos);
}

void append_log(const fs::path& log, const std::string& text) {
    if (log.empty()) {
        return;
    }
    platform::make_directories(log.parent_path());
    std::ofstream out(log, std::ios::binary | std::ios::app);
    out << text;
}

ProcessResult run_step(const ModuleBuildOptions& o, const std::vector<std::string>& args, const char* step) {
    ProcessOptions p;
    p.program = o.cmake;
    p.args = args;
    p.timeout = o.timeout;
    p.cancel = o.cancel;
    p.on_output = o.on_output;
    append_log(o.log_file, "$ " + describe_command(p.program, p.args) + "\n");
    const ProcessResult r = run_process(p);
    append_log(o.log_file, r.out + (r.err.empty() ? "" : "\n--- stderr ---\n" + r.err) +
                               "\n--- exit status " + std::to_string(r.exit_code) + ", " +
                               std::to_string(r.elapsed.count()) + " ms ---\n\n");
    const std::string log_hint =
        o.log_file.empty() ? std::string() : " (full log: " + path_utf8(o.log_file) + ")";
    if (r.timed_out) {
        throw Error(ErrorCode::process_timeout,
                    std::string("cmake ") + step + " ran past its time limit" + log_hint);
    }
    if (r.cancelled) {
        throw Error(ErrorCode::process_cancelled, std::string("cmake ") + step + " was cancelled");
    }
    if (!r.ok()) {
        std::string excerpt = tail_lines(r.out, excerpt_lines);
        const std::string err_tail = tail_lines(r.err, excerpt_lines);
        excerpt += (excerpt.empty() || err_tail.empty() ? "" : "\n") + err_tail;
        throw Error(ErrorCode::module_build_failed, std::string("cmake ") + step + " failed with status " +
                                                        std::to_string(r.exit_code) + log_hint + "\n" +
                                                        excerpt);
    }
    return r;
}

fs::path find_library(const fs::path& build_dir, std::string_view unit) {
    const std::vector<std::string> names = module_file_names(unit);
    for (const std::string& n : names) {
        const fs::path candidate = build_dir / "module" / n; // where cmake/module puts it
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(build_dir, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory() && it->path().filename() == "CMakeFiles") {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file() &&
            std::find(names.begin(), names.end(), path_utf8(it->path().filename())) != names.end()) {
            return it->path();
        }
    }
    return {};
}

} // namespace

unsigned default_build_jobs(unsigned threads, std::optional<std::uint64_t> available_bytes) noexcept {
    const unsigned cores = std::max(threads, 1u);
    if (!available_bytes) {
        return cores;
    }
    if (*available_bytes <= headroom) {
        return 1;
    }
    // (available - 3 GiB) / 0.4 GiB without floating point.
    const std::uint64_t by_memory = (*available_bytes - headroom) * 5 / (2 * gib);
    return static_cast<unsigned>(std::clamp<std::uint64_t>(by_memory, 1, cores));
}

unsigned default_build_jobs() {
    return default_build_jobs(platform::hardware_threads(), platform::available_memory_bytes());
}

std::vector<std::string> module_file_names(std::string_view unit) {
    // cmake/module builds a MODULE library without the "lib" prefix; the others are fallbacks.
    const std::string base = "dq8_unit_" + std::string(unit);
#if defined(_WIN32)
    return {base + ".dll", "lib" + base + ".dll"};
#elif defined(__APPLE__)
    return {base + ".so", base + ".dylib", "lib" + base + ".so", "lib" + base + ".dylib"};
#else
    return {base + ".so", "lib" + base + ".so"};
#endif
}

std::vector<std::string> module_configure_args(const ModuleBuildOptions& o, const ModuleUnit& u) {
    std::vector<std::string> args{
        "-S",
        cmake_path(o.module_source_dir),
        "-B",
        cmake_path(o.build_dir),
        "-G",
        o.cmake_generator,
        "-DCMAKE_BUILD_TYPE=" + o.build_type,
        "-DDQ8_UNIT_NAME=" + u.name,
        "-DDQ8_UNIT_KIND=" + std::string(to_string(u.kind)),
        "-DDQ8_UNIT_OVERLAY_ID=" + std::to_string(u.overlay_id),
        "-DDQ8_UNIT_GENERATED_DIR=" + cmake_path(u.generated_dir),
        "-DDQ8_UNIT_PROFILE_ID=" + u.profile_id,
        "-DDQ8_UNIT_SOURCE_SHA1=" + u.source_sha1,
        "-DDQ8_GENERATOR=" + o.generator_id,
        "-DDQ8_UNIT_ENTRY_POINT=" + hex8(u.entry_point),
        "-DDQ8_ABI_INCLUDE_DIR=" + cmake_path(o.abi_include_dir),
        std::string("-DDQ8_MODULE_PCH=") + (o.pch ? "ON" : "OFF"),
        std::string("-DDQ8_MODULE_DEBUG_INFO=") + (o.debug_info ? "ON" : "OFF"),
    };
    args.insert(args.end(), o.extra_configure_args.begin(), o.extra_configure_args.end());
    return args;
}

std::vector<std::string> module_build_args(const ModuleBuildOptions& o) {
    return {"--build",    cmake_path(o.build_dir),
            "--config",   o.build_type,
            "--parallel", std::to_string(std::max(o.jobs, 1u))};
}

ModuleBuildResult build_module(const ModuleBuildOptions& o, const ModuleUnit& u) {
    std::error_code ec;
    if (!fs::is_regular_file(o.module_source_dir / "CMakeLists.txt", ec)) {
        throw Error(ErrorCode::tool_missing, "the module template '" + path_utf8(o.module_source_dir) +
                                                 "' has no CMakeLists.txt (pass --module-template DIR)");
    }
    if (!fs::is_regular_file(o.abi_include_dir / "dq8" / "abi" / "game_module.h", ec)) {
        throw Error(ErrorCode::tool_missing,
                    "'" + path_utf8(o.abi_include_dir) +
                        "' does not hold dq8/abi/game_module.h (pass --abi-include-dir DIR)");
    }
    if (!fs::is_directory(u.generated_dir, ec)) {
        throw Error(ErrorCode::work_stale, "no generated sources for unit '" + u.name + "' in '" +
                                               path_utf8(u.generated_dir) +
                                               "'; run `dq8ctl recomp generate` first");
    }
    platform::make_directories(o.build_dir);

    ModuleBuildResult result;
    result.jobs = std::max(o.jobs, 1u);
    const ProcessResult configured = run_step(o, module_configure_args(o, u), "configure");
    result.configure_time = configured.elapsed;
    const ProcessResult built = run_step(o, module_build_args(o), "build");
    result.build_time = built.elapsed;
    result.library = find_library(o.build_dir, u.name);
    if (result.library.empty()) {
        throw Error(ErrorCode::module_build_failed, "the build of unit '" + u.name + "' finished but no " +
                                                        module_file_names(u.name).front() +
                                                        " appeared under '" + path_utf8(o.build_dir) + "'");
    }
    return result;
}

} // namespace dq8::core::recomp
