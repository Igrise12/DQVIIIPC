// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/process.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Stage 5 of the setup pipeline: run the pinned recompiler (`ps2_recomp <config.toml>`) on one code unit and
// read the counts from the report it prints ("========== PS2Recomp report =========="). The output directory
// is swapped in only on success, and files whose bytes did not change keep their old modification time, so
// the module build that follows recompiles only what the recompiler really changed.
namespace dq8::core::recomp {

struct RecompEvent {
    std::string severity; // "warning" or "error"
    std::string category; // e.g. "ghidra-map", "control-flow"
    std::string text;     // the whole event line, trimmed
};

struct RecompReport {
    bool found = false; // the report block was present
    // Every "label: N" of the report, in print order, keyed by a normalised label: functions_discovered,
    // symbols, sections, relocations, functions_processed, functions_recompiled, functions_stubbed,
    // functions_skipped, decode_failures, additional_entrypoints, generated_functions,
    // indirect_fallback_promotions, indirect_fallback_entries, unhandled_instructions,
    // correctness_critical_guest_fallbacks, correctness_critical_failures, warnings, errors.
    std::vector<std::pair<std::string, std::uint64_t>> counts;
    std::map<std::string, std::uint64_t> events_by_category; // "<severity>:<category>" -> lines
    // Warnings and errors other than unhandled instructions (the first 50).
    std::vector<RecompEvent> notable_events;

    std::uint64_t count(std::string_view key) const noexcept; // 0 when absent
};

// Parses the report from the recompiler's stdout. Lines outside the report block are ignored.
RecompReport parse_recomp_report(std::string_view output);

struct GenerateOptions {
    std::filesystem::path ps2_recomp; // the recompiler executable
    std::filesystem::path config;     // the unit's TOML
    std::filesystem::path output_dir; // the config's [general].output
    std::filesystem::path log_file;   // receives the full stdout and stderr; empty: none
    std::chrono::milliseconds timeout{0};
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(OutputStream, std::string_view)> on_output;
};

struct GenerateResult {
    RecompReport report;
    std::chrono::milliseconds elapsed{0};
    std::size_t files = 0;           // in the new output directory
    std::size_t files_unchanged = 0; // identical to the previous run's (modification time kept)
    std::size_t files_removed = 0;   // present before, gone now
    std::uint64_t bytes = 0;
};

// Runs the recompiler. The previous output (if any) is moved aside first and restored when the run fails.
// Throws Error(process_spawn) when the program cannot start, process_timeout / process_cancelled, and
// recomp_failed for a non-zero exit or a missing report (the message carries the last lines of output and
// the log path).
GenerateResult run_recompiler(const GenerateOptions& options);

} // namespace dq8::core::recomp
