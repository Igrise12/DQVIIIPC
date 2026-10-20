// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A portable subprocess runner for the setup pipeline (the recompiler, CMake). An argument vector, never a
// shell; an optional working directory and extra environment; stdout and stderr captured separately and
// streamed to a callback as they arrive; a time limit and cooperative cancellation. POSIX uses posix_spawn
// with the child leading its own process group, Windows CreateProcessW inside a job object, so a timeout or a
// cancel stops the whole tree (cmake -> ninja -> compilers), not only the direct child. stdin is the null
// device.
namespace dq8::core {

enum class OutputStream { out, err };

struct ProcessOptions {
    std::filesystem::path program;                        // a path, or a bare name looked up in PATH
    std::vector<std::string> args;                        // argv[1..], passed verbatim (UTF-8)
    std::filesystem::path working_dir;                    // empty: inherit the caller's
    std::vector<std::pair<std::string, std::string>> env; // variables to set or override in the child
    std::chrono::milliseconds timeout{0};                 // 0: no limit
    const std::atomic<bool>* cancel = nullptr;            // polled; true stops the process tree
    // Output as it arrives, on the calling thread, in arbitrary chunks (not lines).
    std::function<void(OutputStream, std::string_view)> on_output;
    std::size_t capture_limit = std::size_t{64} << 20; // bytes kept per stream; the callback sees everything
};

struct ProcessResult {
    int exit_code = -1;     // the exit status; -1 when the process was ended by a signal or stopped by us
    int signal = 0;         // POSIX: the signal that ended it, else 0
    bool timed_out = false; // stopped because the time limit passed
    bool cancelled = false; // stopped because *cancel became true
    std::string out;        // captured stdout (up to capture_limit)
    std::string err;        // captured stderr (up to capture_limit)
    bool out_truncated = false;
    bool err_truncated = false;
    std::chrono::milliseconds elapsed{0};

    bool ok() const noexcept { return exit_code == 0 && signal == 0 && !timed_out && !cancelled; }
};

// Runs the program to completion. Throws Error(process_spawn) when it cannot be found or started. A
// non-zero exit, a timeout or a cancellation is reported in the result rather than thrown, so the caller can
// show the output gathered so far.
ProcessResult run_process(const ProcessOptions& options);

// Resolves a program the way a shell would: a name with a directory part is checked as given; a bare name
// is searched for in PATH (trying PATHEXT extensions on Windows). Returns an absolute path to an executable
// file, or nullopt.
std::optional<std::filesystem::path> find_program(const std::filesystem::path& name);

// A readable one-line rendering of a command for logs and error messages (POSIX-shell-like quoting). Not
// meant to be executed.
std::string describe_command(const std::filesystem::path& program, const std::vector<std::string>& args);

// One argument quoted by the Windows rules (CommandLineToArgvW and the MSVC runtime parse it back to
// `arg`). Used to build CreateProcessW command lines; exposed for tests.
std::string quote_windows_argument(std::string_view arg);

} // namespace dq8::core
