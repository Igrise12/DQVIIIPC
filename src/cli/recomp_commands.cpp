// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// dq8ctl recomp and make-unit-elf: stages 3 to 6 of the setup pipeline (architecture.md §3) over libdq8core.
#include "cli.h"
#include "dq8/core/elf_headers.h"
#include "dq8/core/intake.h"
#include "dq8/core/mwo3.h"
#include "dq8/core/overlay_elf.h"
#include "dq8/core/recomp.h"
#include "dq8/core/region_profile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <optional>

namespace dq8::cli {

namespace {

namespace recomp = dq8::core::recomp;

// Ctrl-C (and SIGTERM) cancel the running stage: the process runner stops the whole tool tree, so no
// compiler is left behind. A lock-free atomic store is signal-safe.
std::atomic<bool> interrupted{false};

void on_interrupt(int) {
    interrupted.store(true);
}

std::string value_or(const Args& args, const char* key, const std::string& fallback) {
    const auto it = args.values.find(key);
    return it == args.values.end() ? fallback : it->second;
}

std::vector<std::string> split_list(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find(',', start), text.size());
        std::string item = text.substr(start, end - start);
        item.erase(0, item.find_first_not_of(' '));
        item.erase(item.find_last_not_of(' ') + 1);
        if (!item.empty()) {
            out.push_back(item);
        }
        start = end + 1;
    }
    return out;
}

unsigned parse_jobs(const std::string& text) {
    unsigned long n = 0;
    std::size_t used = 0;
    try {
        n = std::stoul(text, &used, 10);
    } catch (const std::exception&) {
        used = 0;
    }
    if (used != text.size() || n == 0 || n > 1024) {
        throw Error(ErrorCode::invalid_argument, "--jobs needs a number from 1 to 1024, not '" + text + "'");
    }
    return static_cast<unsigned>(n);
}

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// A data directory of the source tree or the install: `relative` under the current directory, under an
// ancestor of the executable (the build tree lives inside the source tree), or under share/dq8 beside one,
// accepted when it contains `marker`. Empty when none does.
fs::path find_data_dir(const char* argv0, const fs::path& relative, const fs::path& marker) {
    std::vector<fs::path> candidates{relative};
    fs::path dir = executable_dir(argv0);
    for (int up = 0; up < 6 && !dir.empty(); ++up) {
        candidates.push_back(dir / relative);
        candidates.push_back(dir / "share" / "dq8" / relative);
        if (dir == dir.parent_path()) {
            break;
        }
        dir = dir.parent_path();
    }
    std::error_code ec;
    for (const fs::path& c : candidates) {
        if (fs::exists(c / marker, ec)) {
            return c;
        }
    }
    return {};
}

fs::path module_template_dir(const Args& args, const char* argv0) {
    if (const auto it = args.values.find("module-template"); it != args.values.end()) {
        return it->second;
    }
    const fs::path found = find_data_dir(argv0, fs::path("cmake") / "module", "CMakeLists.txt");
    return found.empty() ? fs::path("cmake") / "module" : found; // core reports tool_missing with the path
}

fs::path abi_include_dir(const Args& args, const char* argv0) {
    if (const auto it = args.values.find("abi-include"); it != args.values.end()) {
        return it->second;
    }
    const fs::path found = find_data_dir(argv0, "include", fs::path("dq8") / "abi" / "game_module.h");
    return found.empty() ? fs::path("include") : found;
}

// --ps2-recomp, else the recompiler this build produced, else one installed beside dq8ctl, else PATH.
fs::path ps2_recomp_path(const Args& args, const char* argv0) {
    if (const auto it = args.values.find("ps2-recomp"); it != args.values.end()) {
        return it->second;
    }
#ifdef DQ8_DEFAULT_PS2_RECOMP
    std::error_code ec;
    if (fs::is_regular_file(DQ8_DEFAULT_PS2_RECOMP, ec)) {
        return DQ8_DEFAULT_PS2_RECOMP;
    }
#endif
    const fs::path beside = executable_dir(argv0) / "ps2_recomp";
    if (const auto found = dq8::core::find_program(beside)) {
        return *found;
    }
    return "ps2_recomp";
}

double secs(std::chrono::milliseconds ms) {
    return static_cast<double>(ms.count()) / 1000.0;
}

void json_counts(Json& j, const recomp::RecompReport& r) {
    j.key("counts");
    j.begin_object();
    for (const auto& [k, v] : r.counts) {
        j.member(k, v);
    }
    j.end_object();
}

void report_prepare(const recomp::PrepareReport& r, Json* j, std::FILE* out) {
    if (j != nullptr) {
        j->key("prepare");
        j->begin_object();
        j->member("units_file", utf8(r.units_file));
        j->key("units");
        j->begin_array();
        for (const recomp::PreparedUnit& p : r.units) {
            j->begin_object();
            j->member("name", p.unit.name);
            j->member("kind", recomp::to_string(p.unit.kind));
            j->member("overlay_id", p.unit.overlay_id);
            j->member("source_path", p.unit.source_path);
            j->member("source_sha1", p.source_sha1);
            j->member("code_start", hex32(p.unit.code_start));
            j->member("code_end", hex32(p.unit.code_end));
            j->member("elf", utf8(p.elf));
            j->member("elf_written", p.elf_written);
            j->member("config", utf8(p.config));
            j->member("config_written", p.config_written);
            j->member("map", utf8(p.map_csv));
            j->member("functions", static_cast<std::uint64_t>(p.functions));
            j->member("stubs", static_cast<std::uint64_t>(p.stubs));
            j->member("entry_points", static_cast<std::uint64_t>(p.entry_points));
            j->member("jump_tables", static_cast<std::uint64_t>(p.jump_tables));
            j->key("warnings");
            j->begin_array();
            for (const std::string& w : p.warnings) {
                j->value(w);
            }
            j->end_array();
            j->end_object();
        }
        j->end_array();
        j->end_object();
        return;
    }
    std::fprintf(out, "prepare:  %zu units -> %s\n", r.units.size(),
                 utf8(r.units_file.parent_path()).c_str());
    for (const recomp::PreparedUnit& p : r.units) {
        std::fprintf(
            out,
            "  %-8s %s..%s  ELF %-9s config %-9s %zu functions, %zu stubs, %zu entry points, %zu jump "
            "tables\n",
            p.unit.name.c_str(), hex32(p.unit.code_start).c_str(), hex32(p.unit.code_end).c_str(),
            p.elf_written ? "written" : "unchanged", p.config_written ? "written" : "unchanged", p.functions,
            p.stubs, p.entry_points, p.jump_tables);
        for (const std::string& w : p.warnings) {
            std::fprintf(out, "           warning: %s\n", w.c_str());
        }
    }
}

void report_generate(const recomp::GenerateReport& r, Json* j, std::FILE* out) {
    if (j != nullptr) {
        j->key("generate");
        j->begin_object();
        j->member("ps2_recomp", utf8(r.ps2_recomp));
        j->member("generator", r.generator_id);
        j->key("units");
        j->begin_array();
        for (const recomp::GeneratedUnit& g : r.units) {
            j->begin_object();
            j->member("name", g.name);
            j->member("skipped", g.skipped);
            j->member("elapsed_ms", static_cast<std::int64_t>(g.result.elapsed.count()));
            j->member("files", static_cast<std::uint64_t>(g.result.files));
            j->member("files_unchanged", static_cast<std::uint64_t>(g.result.files_unchanged));
            j->member("files_removed", static_cast<std::uint64_t>(g.result.files_removed));
            j->member("bytes", g.result.bytes);
            j->member("output", utf8(g.output_dir));
            j->member("log", utf8(g.log_file));
            json_counts(*j, g.result.report);
            j->key("notable_events");
            j->begin_array();
            for (const recomp::RecompEvent& e : g.result.report.notable_events) {
                j->value(e.text);
            }
            j->end_array();
            j->end_object();
        }
        j->end_array();
        j->end_object();
        return;
    }
    std::fprintf(out, "generate: %s (%s)\n", utf8(r.ps2_recomp).c_str(), r.generator_id.c_str());
    for (const recomp::GeneratedUnit& g : r.units) {
        const recomp::RecompReport& rep = g.result.report;
        std::fprintf(
            out,
            "  %-8s %s %.1f s: %zu files (%zu unchanged, %zu removed); functions %llu discovered, %llu "
            "recompiled, %llu stubs, %llu skipped; %llu unhandled instructions\n",
            g.name.c_str(), g.skipped ? "up to date, last run" : "ran", secs(g.result.elapsed),
            g.result.files, g.result.files_unchanged, g.result.files_removed,
            static_cast<unsigned long long>(rep.count("functions_discovered")),
            static_cast<unsigned long long>(rep.count("functions_recompiled")),
            static_cast<unsigned long long>(rep.count("functions_stubbed")),
            static_cast<unsigned long long>(rep.count("functions_skipped")),
            static_cast<unsigned long long>(rep.count("unhandled_instructions")));
        std::size_t shown = 0;
        for (const recomp::RecompEvent& e : rep.notable_events) {
            if (++shown > 5) {
                std::fprintf(out, "           ... (%zu more in %s)\n", rep.notable_events.size() - 5,
                             utf8(g.log_file).c_str());
                break;
            }
            std::fprintf(out, "           %s\n", e.text.c_str());
        }
    }
}

void report_build(const recomp::BuildReport& r, Json* j, std::FILE* out) {
    if (j != nullptr) {
        j->key("build");
        j->begin_object();
        j->member("modules_dir", utf8(r.modules_dir));
        j->member("manifest", utf8(r.manifest));
        j->member("jobs", r.jobs);
        j->key("units");
        j->begin_array();
        for (const recomp::BuiltUnit& b : r.units) {
            j->begin_object();
            j->member("name", b.name);
            j->member("module", utf8(b.module));
            j->member("updated", b.module_updated);
            j->member("configure_ms", static_cast<std::int64_t>(b.result.configure_time.count()));
            j->member("build_ms", static_cast<std::int64_t>(b.result.build_time.count()));
            j->member("log", utf8(b.log_file));
            j->end_object();
        }
        j->end_array();
        j->end_object();
        return;
    }
    std::fprintf(out, "build:    %u jobs; modules in %s\n", r.jobs, utf8(r.modules_dir).c_str());
    for (const recomp::BuiltUnit& b : r.units) {
        std::fprintf(out, "  %-8s %s %s (configure %.1f s, build %.1f s)\n", b.name.c_str(),
                     utf8(b.module.filename()).c_str(), b.module_updated ? "updated" : "unchanged",
                     secs(b.result.configure_time), secs(b.result.build_time));
    }
    std::fprintf(out, "manifest: %s\n", utf8(r.manifest).c_str());
}

std::vector<std::uint8_t> read_small_file(const fs::path& path, std::uint64_t limit) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        throw Error(ErrorCode::io_open, "cannot open '" + utf8(path) + "'");
    }
    if (size > limit) {
        throw Error(ErrorCode::io_read,
                    "'" + utf8(path) + "' is larger than " + std::to_string(limit) + " bytes");
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in || static_cast<std::uintmax_t>(in.gcount()) != size) {
        throw Error(ErrorCode::io_read, "short read on '" + utf8(path) + "'");
    }
    return bytes;
}

} // namespace

int run_recomp(const Args& args, const char* argv0) {
    const bool json = args.flags.count("json") != 0;
    const std::vector<std::string> stages{"prepare", "generate", "build", "all"};
    if (args.positional.size() != 2 ||
        std::find(stages.begin(), stages.end(), args.positional[0]) == stages.end()) {
        std::fputs("dq8ctl recomp: expected a stage (prepare, generate, build or all) and an <iso>\n",
                   stderr);
        return usage(stderr);
    }
    const std::string stage = args.positional[0];
    const bool all = stage == "all";

    recomp::PipelineOptions o;
    o.work_dir = value_or(args, "work", (fs::path("work") / "recomp").string());
    o.maps_dir = value_or(args, "maps", (o.work_dir / "maps").string());
    o.units = split_list(value_or(args, "units", ""));
    if (const auto it = args.values.find("jobs"); it != args.values.end()) {
        o.jobs = parse_jobs(it->second);
    }
    o.force = args.flags.count("force") != 0;
    o.pch = args.flags.count("no-pch") == 0;
    o.debug_info = args.flags.count("debug-info") != 0;
    o.cmake = value_or(args, "cmake", "cmake");
    o.cmake_generator = value_or(args, "generator", "Ninja");
    o.cancel = &interrupted;
    std::signal(SIGINT, on_interrupt);
    std::signal(SIGTERM, on_interrupt);
    // With --json, stdout carries exactly one object; progress goes to stderr.
    std::FILE* log = json ? stderr : stdout;
    o.on_progress = [log](const std::string& unit, const std::string& message) {
        std::fprintf(log, "[%s] %s\n", unit.c_str(), message.c_str());
        std::fflush(log);
    };
    if (args.flags.count("verbose") != 0) {
        o.on_tool_output = [](dq8::core::OutputStream, std::string_view chunk) {
            std::fwrite(chunk.data(), 1, chunk.size(), stderr);
        };
    }

    const auto profiles = dq8::core::load_region_profiles(regions_dir(args, argv0));
    const dq8::core::VerifiedImage v = dq8::core::verify_image(args.positional[1], profiles);
    Json j;
    Json* jp = json ? &j : nullptr;
    j.begin_object();
    j.member("ok", true);
    j.member("stage", stage);
    j.member("profile", v.profile.id);
    j.member("work", utf8(o.work_dir));
    if (!json) {
        print_warnings(v.warnings, stdout);
        std::printf("profile:  %s  work: %s\n", v.profile.id.c_str(), utf8(o.work_dir).c_str());
    }
    if (all || stage == "prepare") {
        report_prepare(recomp::prepare_units(v, o), jp, stdout);
    }
    if (all || stage == "generate") {
        o.ps2_recomp = ps2_recomp_path(args, argv0);
        report_generate(recomp::generate_units(v, o), jp, stdout);
    }
    if (all || stage == "build") {
        o.module_source_dir = module_template_dir(args, argv0);
        o.abi_include_dir = abi_include_dir(args, argv0);
        report_build(recomp::build_units(v, o), jp, stdout);
    }
    j.end_object();
    if (json) {
        std::printf("%s\n", j.str().c_str());
    }
    return exit_ok;
}

int run_make_unit_elf(const Args& args, const char* argv0) {
    const bool json = args.flags.count("json") != 0;
    const auto out_it = args.values.find("out");
    if (args.positional.size() != 1 || out_it == args.values.end()) {
        std::fputs("dq8ctl make-unit-elf: expected one <overlay .BIN> and --out FILE\n", stderr);
        return usage(stderr);
    }
    const fs::path bin = args.positional[0];
    const fs::path out = out_it->second;
    const std::vector<std::uint8_t> container = read_small_file(bin, 16u << 20);

    // The window comes from the region profile whose overlay window starts at the header's load address,
    // when one can be found (as `info` does); otherwise the container is checked on its own terms.
    const dq8::core::Mwo3Header self = dq8::core::parse_mwo3_header(container, container.size());
    std::optional<dq8::core::Mwo3Window> window;
    std::string profile_id;
    try {
        for (const dq8::core::RegionProfile& p : dq8::core::load_region_profiles(regions_dir(args, argv0))) {
            if (p.overlay_window.base == self.load_address) {
                window = dq8::core::Mwo3Window{p.overlay_window.base, p.overlay_window.end};
                profile_id = p.id;
                break;
            }
        }
    } catch (const Error&) {
        // No profiles reachable: build from the container alone.
    }
    std::uint32_t elf_flags = 0;
    if (const auto it = args.values.find("boot"); it != args.values.end()) {
        elf_flags = dq8::core::parse_elf_headers(read_small_file(it->second, 64u << 20)).flags;
    }
    const dq8::core::OverlayUnitElf unit = dq8::core::make_overlay_unit_elf(container, elf_flags, window);
    {
        const fs::path tmp = out.string() + ".tmp";
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(unit.elf.data()),
                   static_cast<std::streamsize>(unit.elf.size()));
        file.close();
        std::error_code ec;
        if (!file) {
            throw Error(ErrorCode::io_write, "cannot write '" + utf8(tmp) + "'");
        }
        fs::rename(tmp, out, ec);
        if (ec) {
            throw Error(ErrorCode::io_write, "cannot replace '" + utf8(out) + "': " + ec.message());
        }
    }
    const dq8::core::Mwo3Header& h = unit.header;
    if (json) {
        Json j;
        j.begin_object();
        j.member("ok", true);
        j.member("out", utf8(out));
        j.member("size", static_cast<std::uint64_t>(unit.elf.size()));
        j.member("overlay_id", h.overlay_id);
        j.member("name", h.name);
        j.member("profile", profile_id);
        j.member("text_address", hex32(h.load_address));
        j.member("text_size", hex32(h.file_size()));
        j.member("bss_address", hex32(h.bss_start()));
        j.member("bss_size", hex32(h.bss_size));
        j.member("e_flags", hex32(elf_flags));
        j.end_object();
        std::printf("%s\n", j.str().c_str());
        return exit_ok;
    }
    std::printf("wrote %s (%zu bytes): overlay %u \"%s\"%s\n", utf8(out).c_str(), unit.elf.size(),
                h.overlay_id, h.name.c_str(),
                profile_id.empty() ? " (no region profile owns its window)" : "");
    std::printf("  .text %s..%s  (header, text and data; SHF_ALLOC|SHF_EXECINSTR|SHF_WRITE)\n",
                hex32(h.load_address).c_str(), hex32(h.bss_start()).c_str());
    if (h.bss_size != 0) {
        std::printf("  .bss  %s..%s\n", hex32(h.bss_start()).c_str(), hex32(h.end_address()).c_str());
    }
    std::printf("  e_entry 0, e_flags %s\n", hex32(elf_flags).c_str());
    return exit_ok;
}

} // namespace dq8::cli
