// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/recomp/recomp_driver.h"

#include "dq8/core/error.h"
#include "platform/files.h"
#include "recomp/common.h"

#include <algorithm>

namespace dq8::core::recomp {

namespace fs = std::filesystem;
using detail::path_utf8;

namespace {

constexpr std::size_t max_notable_events = 50;
constexpr std::size_t excerpt_lines = 20;

std::string normalise_label(std::string_view label) {
    std::string key;
    for (const char c : label) {
        if (c >= 'A' && c <= 'Z') {
            key.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if (c == ' ' || c == '-') {
            key.push_back('_');
        } else {
            key.push_back(c);
        }
    }
    while (!key.empty() && key.front() == '_') {
        key.erase(key.begin());
    }
    // Labels that only make sense next to the first one on their line.
    static const std::pair<std::string_view, std::string_view> renames[] = {
        {"recompiled", "functions_recompiled"},
        {"stubs", "functions_stubbed"},
        {"skipped", "functions_skipped"},
        {"fallback_entries", "indirect_fallback_entries"},
        {"failures", "correctness_critical_failures"},
    };
    for (const auto& [from, to] : renames) {
        if (key == from) {
            return std::string(to);
        }
    }
    return key;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

bool read_number(std::string_view& s, std::uint64_t& value) {
    std::size_t n = 0;
    value = 0;
    while (n < s.size() && s[n] >= '0' && s[n] <= '9') {
        value = value * 10 + static_cast<std::uint64_t>(s[n] - '0');
        ++n;
    }
    s.remove_prefix(n);
    return n != 0;
}

// "Label: N, label: N (M more words)" -> (label, N) pairs, including the parenthesised (words, M).
void parse_count_line(std::string_view line, RecompReport& report) {
    std::size_t start = 0;
    while (start < line.size()) {
        std::size_t end = line.find(", ", start);
        if (end == std::string_view::npos) {
            end = line.size();
        }
        std::string_view segment = line.substr(start, end - start);
        start = end + 2;
        const std::size_t colon = segment.find(": ");
        if (colon == std::string_view::npos) {
            continue;
        }
        const std::string_view label = trim(segment.substr(0, colon));
        std::string_view rest = segment.substr(colon + 2);
        std::uint64_t value = 0;
        if (!read_number(rest, value)) {
            continue;
        }
        report.counts.emplace_back(normalise_label(label), value);
        rest = trim(rest);
        if (rest.size() > 2 && rest.front() == '(' && rest.back() == ')') {
            std::string_view inner = rest.substr(1, rest.size() - 2);
            std::uint64_t extra = 0;
            if (read_number(inner, extra)) {
                report.counts.emplace_back(normalise_label(trim(inner)), extra);
            }
        }
    }
}

// "  [warning] ghidra-map function=f addr=0x... - message"
void parse_event_line(std::string_view line, RecompReport& report) {
    const std::string_view text = trim(line);
    const std::size_t close = text.find(']');
    if (text.empty() || text.front() != '[' || close == std::string_view::npos) {
        return;
    }
    RecompEvent event;
    event.severity = std::string(text.substr(1, close - 1));
    std::string_view rest = trim(text.substr(close + 1));
    event.category = std::string(rest.substr(0, rest.find(' ')));
    event.text = std::string(text);
    ++report.events_by_category[event.severity + ":" + event.category];
    const bool notable = event.severity == "warning" || event.severity == "error";
    if (notable && event.category != "unhandled-instruction" &&
        report.notable_events.size() < max_notable_events) {
        report.notable_events.push_back(std::move(event));
    }
}

// The last `count` lines of `text`.
std::string tail_lines(std::string_view text, std::size_t count) {
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
    return std::string(text.substr(pos, end - pos));
}

} // namespace

std::uint64_t RecompReport::count(std::string_view key) const noexcept {
    for (const auto& [k, v] : counts) {
        if (k == key) {
            return v;
        }
    }
    return 0;
}

RecompReport parse_recomp_report(std::string_view output) {
    RecompReport report;
    bool in_block = false;
    bool in_events = false;
    std::size_t pos = 0;
    while (pos < output.size()) {
        std::size_t end = output.find('\n', pos);
        if (end == std::string_view::npos) {
            end = output.size();
        }
        std::string_view line = output.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.starts_with("==========")) {
            if (line.find("PS2Recomp report") != std::string_view::npos) {
                report = RecompReport{};
                report.found = true;
                in_block = true;
                in_events = false;
            } else {
                in_block = false;
            }
            continue;
        }
        if (!in_block) {
            continue;
        }
        if (trim(line) == "Events:") {
            in_events = true;
        } else if (in_events) {
            parse_event_line(line, report);
        } else {
            parse_count_line(line, report);
        }
    }
    return report;
}

GenerateResult run_recompiler(const GenerateOptions& o) {
    const fs::path output = o.output_dir;
    const fs::path previous = output.string() + ".previous";
    std::error_code ec;
    fs::remove_all(previous, ec);
    bool had_previous = false;
    if (fs::exists(output, ec)) {
        fs::rename(output, previous, ec);
        if (ec) {
            throw Error(ErrorCode::io_write,
                        "cannot move '" + path_utf8(output) + "' aside before regenerating: " + ec.message());
        }
        had_previous = true;
    }
    auto restore = [&] {
        std::error_code ignored;
        fs::remove_all(output, ignored);
        if (had_previous) {
            fs::rename(previous, output, ignored);
        }
    };

    ProcessOptions p;
    p.program = o.ps2_recomp;
    p.args = {path_utf8(fs::absolute(o.config))};
    p.working_dir = fs::absolute(o.config).parent_path();
    p.timeout = o.timeout;
    p.cancel = o.cancel;
    p.on_output = o.on_output;
    ProcessResult r;
    try {
        r = run_process(p);
    } catch (...) {
        restore();
        throw;
    }
    if (!o.log_file.empty()) {
        platform::write_file_atomic(o.log_file, "$ " + describe_command(p.program, p.args) + "\n" + r.out +
                                                    "\n--- stderr ---\n" + r.err + "\n--- exit status " +
                                                    std::to_string(r.exit_code) + ", " +
                                                    std::to_string(r.elapsed.count()) + " ms ---\n");
    }
    const std::string log_hint =
        o.log_file.empty() ? std::string() : " (full log: " + path_utf8(o.log_file) + ")";
    if (r.timed_out) {
        restore();
        throw Error(ErrorCode::process_timeout,
                    "ps2_recomp on '" + path_utf8(o.config) + "' ran past its time limit" + log_hint);
    }
    if (r.cancelled) {
        restore();
        throw Error(ErrorCode::process_cancelled,
                    "ps2_recomp on '" + path_utf8(o.config) + "' was cancelled");
    }

    GenerateResult result;
    result.report = parse_recomp_report(r.out);
    result.elapsed = r.elapsed;
    if (!r.ok() || !result.report.found || !fs::is_directory(output, ec)) {
        restore();
        const std::string why = !r.ok()                ? "exited with status " + std::to_string(r.exit_code)
                                : !result.report.found ? "printed no report"
                                                       : "wrote no output directory";
        std::string excerpt = tail_lines(r.err, excerpt_lines);
        const std::string out_tail = tail_lines(r.out, excerpt_lines);
        excerpt += (excerpt.empty() || out_tail.empty() ? "" : "\n") + out_tail;
        throw Error(ErrorCode::recomp_failed,
                    "ps2_recomp on '" + path_utf8(o.config) + "' " + why + log_hint + "\n" + excerpt);
    }

    // Keep the modification time of every file whose bytes did not change, so the module build that follows
    // only recompiles what the recompiler really changed.
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(output)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        ++result.files;
        result.bytes += entry.file_size();
        if (!had_previous) {
            continue;
        }
        const fs::path old = previous / fs::relative(entry.path(), output);
        if (platform::same_contents(old, entry.path())) {
            fs::last_write_time(entry.path(), fs::last_write_time(old), ec);
            ++result.files_unchanged;
        }
    }
    if (had_previous) {
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(previous)) {
            if (entry.is_regular_file() && !fs::exists(output / fs::relative(entry.path(), previous), ec)) {
                ++result.files_removed;
            }
        }
        fs::remove_all(previous, ec);
    }
    return result;
}

} // namespace dq8::core::recomp
