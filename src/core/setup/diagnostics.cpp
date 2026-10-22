// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The diagnostics report (dq8/core/diagnostics.h). Every section is gathered on its own, so a broken part
// (no home directory, a damaged settings file) shows up as a line in the report instead of failing it.
#include "dq8/core/diagnostics.h"

#include "dq8/core/error.h"
#include "dq8/core/registry.h"
#include "dq8/core/settings.h"
#include "dq8/core/setup.h"
#include "dq8/core/toolchain.h"
#include "dq8/core/version.h"
#include "platform/files.h"
#include "platform/paths.h"
#include "platform/system_info.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <system_error>

namespace dq8::core::diagnostics {

namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string gib(std::uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f GiB", static_cast<double>(bytes) / static_cast<double>(1u << 30));
    return buf;
}

// Runs one section; a failure becomes a line of the report.
void section(std::string& out, const char* title, const std::function<void(std::string&)>& body) {
    out += "\n[" + std::string(title) + "]\n";
    try {
        body(out);
    } catch (const std::exception& e) {
        out += "unavailable: " + std::string(e.what()) + "\n";
    }
}

std::string free_space(const fs::path& dir) {
    fs::path p = dir;
    std::error_code ec;
    while (!p.empty() && !fs::exists(p, ec) && p != p.parent_path()) {
        p = p.parent_path();
    }
    const fs::space_info s = fs::space(p, ec);
    return ec ? std::string("unknown") : gib(s.available) + " free";
}

void tool_line(std::string& out, const char* name, const toolchain::Tool& t) {
    out += std::string(name) + ": " +
           (t.found ? utf8(t.path) + (t.version.empty() ? " (no version)" : " " + t.version) : "not found") +
           "\n";
}

} // namespace

std::string report(const Options& o) {
    std::string out = "DQ8-Native diagnostics\n";
    out += "version: " + std::string(version()) + "\n";
    out += "generated: " + utc_now() + "\n";
    out += "contents: system, toolchain, settings, builds, setup log; no game data (see "
           "dq8/core/diagnostics.h)\n";

    fs::path work = o.work_dir;
    section(out, "system", [&](std::string& s) {
        const toolchain::Distribution d = toolchain::this_distribution();
        s += "os: " + d.pretty_name +
             (d.id.empty() ? "" : " (" + d.id + (d.family.empty() ? "" : ", " + d.family) + ")") + "\n";
#if defined(__x86_64__) || defined(_M_X64)
        s += "architecture: x86_64\n";
#elif defined(__aarch64__) || defined(_M_ARM64)
        s += "architecture: arm64\n";
#endif
        s += "hardware threads: " + std::to_string(platform::hardware_threads()) + "\n";
        const auto memory = platform::available_memory_bytes();
        s += "memory available: " + (memory ? gib(*memory) : std::string("unknown")) + "\n";
        s += "default build jobs: " + std::to_string(toolchain::default_jobs()) + "\n";
        try {
            const fs::path data = platform::user_data_dir();
            s += "user data: " + utf8(data) + " (" + free_space(data) + ")\n";
            s += "user config: " + utf8(platform::user_config_dir()) + "\n";
        } catch (const Error& e) {
            s += "user data: " + std::string(e.what()) + "\n";
        }
        if (work.empty()) {
            work = setup::default_work_dir();
        }
        s += "work directory: " + utf8(work) + " (" + free_space(work) + ")\n";
    });

    section(out, "toolchain", [&](std::string& s) {
        toolchain::DetectOptions d;
        d.executable_dir = o.executable_dir;
        const toolchain::Toolchain t = toolchain::detect(d);
        tool_line(s, "c++", t.cxx);
        tool_line(s, "cc", t.cc);
        tool_line(s, "cmake", t.cmake);
        tool_line(s, "ninja", t.ninja);
        s += "compiler id: " + (t.compiler_id.empty() ? std::string("unknown") : t.compiler_id) +
             (t.bundled ? " (bundled)" : "") + "\n";
        const std::vector<toolchain::Problem> problems = toolchain::check(t);
        s += "problems: " + std::string(problems.empty() ? "none" : "") + "\n";
        for (const toolchain::Problem& p : problems) {
            s += "  " + std::string(core::to_string(p.code)) + " " + p.tool + ": " + p.advice + "\n";
        }
    });

    section(out, "settings", [&](std::string& s) {
        const fs::path file =
            o.settings_file.empty() ? settings::default_user_file(o.executable_dir) : o.settings_file;
        std::error_code ec;
        s += "file: " + utf8(file) + (fs::exists(file, ec) ? "" : " (not written yet: the defaults apply)") +
             "\n";
        settings::Store store(
            settings::Schema::load(settings::find_settings_dir(o.executable_dir) / "schema.toml"));
        store.load(settings::Layer::user, file);
        for (const settings::Setting& setting : store.schema().settings()) {
            s += setting.key + " = " + settings::to_text(store.get(setting.key)) + " (" +
                 std::string(settings::to_string(store.source(setting.key))) + ")\n";
        }
        for (const std::string& w : store.warnings()) {
            s += "warning: " + w + "\n";
        }
    });

    std::vector<registry::Install> installs;
    section(out, "builds", [&](std::string& s) {
        const fs::path file = o.registry_file.empty() ? registry::default_file() : o.registry_file;
        s += "registry: " + utf8(file) + "\n";
        installs = registry::Registry::load(file).installs();
        if (installs.empty()) {
            s += "no game is set up yet\n";
        }
        for (const registry::Install& i : installs) {
            std::error_code ec;
            const fs::path manifest = i.module_dir / "build-manifest.toml";
            const bool present = fs::exists(manifest, ec);
            const std::string sha1 = present ? platform::sha1_file(manifest) : std::string();
            s += i.profile_id + ": modules " + utf8(i.module_dir) + ", disc image " + utf8(i.iso) +
                 ", built " + i.built_at + " by " + i.host_version + "\n";
            s += "  manifest " +
                 (present
                      ? sha1 + (sha1 == i.manifest_sha1 ? " (as registered)" : " (changed since registered)")
                      : std::string("missing")) +
                 "\n";
        }
    });
    for (const registry::Install& i : installs) {
        const std::string title = "build manifest " + i.profile_id;
        section(out, title.c_str(), [&](std::string& s) {
            // Our own record: hashes, counts, timings and names; no game content. Indented, so that its
            // [[units]] headers do not read as sections of the report.
            const std::string text = platform::read_text_file(i.module_dir / "build-manifest.toml", 1u << 20);
            std::size_t start = 0;
            while (start < text.size()) {
                const std::size_t end = std::min(text.find('\n', start), text.size());
                s += "  " + text.substr(start, end - start) + "\n";
                start = end + 1;
            }
        });
    }

    section(out, "setup log", [&](std::string& s) {
        const fs::path log = work / "logs" / "setup.log";
        std::ifstream in(log);
        if (!in) {
            s += "no setup log in " + utf8(log) + "\n";
            return;
        }
        // Event lines only: continuation lines ("  | ") can quote tool output.
        std::deque<std::string> lines;
        std::string line;
        std::size_t left_out = 0;
        while (std::getline(in, line)) {
            if (line.rfind("  | ", 0) == 0) {
                ++left_out;
                continue;
            }
            lines.push_back(line);
            if (lines.size() > o.log_lines) {
                lines.pop_front();
            }
        }
        s += "file: " + utf8(log) + " (last " + std::to_string(lines.size()) + " event lines; " +
             std::to_string(left_out) + " lines of tool output left out)\n";
        for (const std::string& l : lines) {
            s += l + "\n";
        }
        s += "tool logs (contents left out):\n";
        std::error_code ec;
        std::vector<fs::path> logs;
        for (auto it = fs::directory_iterator(work / "logs", ec); !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            if (it->is_regular_file() && it->path().filename() != "setup.log") {
                logs.push_back(it->path());
            }
        }
        std::sort(logs.begin(), logs.end());
        for (const fs::path& p : logs) {
            std::error_code size_ec;
            s += "  " + utf8(p.filename()) + " " + std::to_string(fs::file_size(p, size_ec)) + " bytes\n";
        }
    });
    return out;
}

void write_report(const Options& options, const fs::path& file) {
    platform::write_file_atomic(file, report(options));
}

} // namespace dq8::core::diagnostics
