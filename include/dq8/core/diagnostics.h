// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <filesystem>
#include <string>
#include <vector>

// The diagnostics export (architecture.md §11.2, M5): one text report a user can attach to a bug report, for
// `dq8ctl diagnose` and the launcher's Diagnostics page. It holds the program version, system facts (OS,
// threads, memory, free space), the toolchain and its problems, the effective settings with their sources,
// the build registry, each registered build's manifest (hashes, counts, timings), the setup log's event
// lines, and the names and sizes of the per-unit tool logs. It never holds game data: no file from the disc,
// no generated C++, no tool output (compiler messages can quote generated code, so the setup log's "  | "
// continuation lines and the tool logs' contents stay out).
namespace dq8::core::diagnostics {

struct Options {
    std::filesystem::path executable_dir; // for the settings schema and a portable settings file
    std::filesystem::path settings_file;  // empty: settings::default_user_file(executable_dir)
    std::filesystem::path registry_file;  // empty: registry::default_file()
    std::filesystem::path work_dir;       // empty: setup::default_work_dir()
    std::size_t log_lines = 400;          // the last lines of the setup log
};

// The report. Never throws for a missing or unreadable part: the section says what was not there.
std::string report(const Options& options);

// Writes report() atomically to `file`.
void write_report(const Options& options, const std::filesystem::path& file);

} // namespace dq8::core::diagnostics
