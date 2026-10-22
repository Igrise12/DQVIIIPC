// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The first-run setup (dq8/core/setup.h): the stages of architecture.md §3 over the intake and recompile
// pipeline code, with one event stream, cancellation and resumption.
#include "dq8/core/setup.h"

#include "dq8/core/intake.h"
#include "dq8/core/process.h"
#include "dq8/core/recomp.h"
#include "dq8/core/region_profile.h"
#include "dq8/core/registry.h"
#include "dq8/core/toolchain.h"
#include "dq8/core/version.h"
#include "platform/files.h"
#include "platform/paths.h"
// Session E's toolchain download (Windows; ADR-0018), once its header is in the tree.
#if defined(_WIN32) && __has_include("toolchain/download.h")
#include "toolchain/download.h"
#define DQ8_SETUP_HAVE_DOWNLOAD 1
#else
#define DQ8_SETUP_HAVE_DOWNLOAD 0
#endif

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <toml.hpp>
#include <utility>
#include <vector>

namespace dq8::core::setup {

namespace {

namespace fs = std::filesystem;

void append_json_string(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

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
    std::snprintf(buf, sizeof buf, "%.1f GB", static_cast<double>(bytes) / 1e9);
    return buf;
}

// The bytes the regular files under `dir` hold (0 when it does not exist).
std::uint64_t directory_bytes(const fs::path& dir) {
    std::uint64_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code size_ec;
        if (it->is_regular_file(size_ec)) {
            const std::uintmax_t size = it->file_size(size_ec);
            total += size_ec ? 0 : static_cast<std::uint64_t>(size);
        }
    }
    return total;
}

fs::path existing_ancestor(fs::path p) {
    std::error_code ec;
    p = fs::absolute(p, ec);
    while (!p.empty() && !fs::exists(p, ec)) {
        if (p == p.parent_path()) {
            break;
        }
        p = p.parent_path();
    }
    return p;
}

fs::path program_name(std::string_view name) {
#if defined(_WIN32)
    return fs::path(std::string(name) + ".exe");
#else
    return fs::path(std::string(name));
#endif
}

// `given`, else `name` beside the executable, else `name` on PATH; empty when none resolves.
fs::path locate_program(const fs::path& given, const fs::path& executable_dir, std::string_view name) {
    std::vector<fs::path> candidates;
    if (!given.empty()) {
        candidates.push_back(given);
    } else {
        if (!executable_dir.empty()) {
            candidates.push_back(executable_dir / program_name(name));
        }
        candidates.emplace_back(std::string(name));
    }
    for (const fs::path& c : candidates) {
        if (const auto found = find_program(c)) {
            return *found;
        }
    }
    return {};
}

// The last `n` lines of `text`.
std::string tail_lines(const std::string& text, std::size_t n) {
    std::size_t pos = text.size();
    while (pos > 0 && (text[pos - 1] == '\n' || text[pos - 1] == '\r')) {
        --pos;
    }
    const std::size_t end = pos;
    for (std::size_t lines = 0; pos > 0; --pos) {
        if (text[pos - 1] == '\n' && ++lines == n) {
            break;
        }
    }
    return text.substr(pos, end - pos);
}

// A failure inside a stage, with the code the stage ends on.
class StageError : public std::runtime_error {
  public:
    StageError(ErrorCode code, const std::string& message) : std::runtime_error(message), code_(code) {}
    ErrorCode code() const noexcept { return code_; }

  private:
    ErrorCode code_;
};

class Runner {
  public:
    explicit Runner(const Options& o) : o_(o) {}

    Result run() {
        Result result;
        try {
            resolve();
            result.work_dir = work_;
            result.log_file = log_path_;
            stage_verify();
            stage_extract();
            stage_toolchain();
            stage_maps();
            stage_prepare();
            stage_generate();
            stage_build();
            stage_smoke();
            stage_register();
        } catch (const StageError& e) {
            return fail(result, e.code(), e.what());
        } catch (const Error& e) {
            const ErrorCode code =
                e.code() == ErrorCode::process_cancelled ? ErrorCode::setup_cancelled : e.code();
            return fail(result, code,
                        code == ErrorCode::setup_cancelled ? "the setup was cancelled" : e.what());
        } catch (const std::exception& e) {
            return fail(result, ErrorCode::invalid_argument, e.what());
        }
        result.ok = true;
        result.profile_id = profile_id_;
        result.module_dir = modules_dir_;
        result.message = "set up " + profile_id_ + " in " + utf8(modules_dir_);
        log_line("setup finished: " + result.message);
        return result;
    }

  private:
    // --- events and the log ---

    void emit(const Event& e) {
        log_event(e);
        if (o_.on_event) {
            o_.on_event(e);
        }
    }

    void start(Stage stage, const std::string& message) {
        current_ = stage;
        started_ = true;
        last_percent_.clear();
        Event e;
        e.stage = stage;
        e.state = State::start;
        e.message = message;
        emit(e);
        check_cancel();
    }

    // Progress, thinned to percent steps per unit (every call when the total is unknown).
    void progress(std::uint64_t done, std::uint64_t total, const std::string& unit,
                  const std::string& message) {
        if (total != 0) {
            const int percent = static_cast<int>(std::min<std::uint64_t>(done * 100 / total, 100));
            const auto it = last_percent_.find(unit);
            if (it != last_percent_.end() && it->second == percent && done != total && message.empty()) {
                return;
            }
            last_percent_[unit] = percent;
        }
        Event e;
        e.stage = current_;
        e.state = State::progress;
        e.done = done;
        e.total = total;
        e.unit = unit;
        e.message = message;
        emit(e);
    }

    void finish(State state, const std::string& message, std::uint64_t done = 0, std::uint64_t total = 0) {
        Event e;
        e.stage = current_;
        e.state = state;
        e.done = done;
        e.total = total;
        e.message = message;
        emit(e);
        started_ = false;
    }

    Result& fail(Result& result, ErrorCode code, const std::string& message) {
        if (!started_) {
            // A failure before the stage announced itself (path resolution): report it on that stage.
            Event s;
            s.stage = current_;
            s.state = State::start;
            emit(s);
        }
        Event e;
        e.stage = current_;
        e.state = State::error;
        e.code = code;
        e.message = message;
        emit(e);
        result.ok = false;
        result.code = code;
        result.message = message;
        result.profile_id = profile_id_;
        return result;
    }

    void check_cancel() const {
        if (o_.cancel != nullptr && o_.cancel->load()) {
            throw StageError(ErrorCode::setup_cancelled, "the setup was cancelled");
        }
    }

    void open_log() {
        std::error_code ec;
        fs::create_directories(log_path_.parent_path(), ec);
        if (fs::exists(log_path_, ec)) {
            fs::rename(log_path_, log_path_.parent_path() / "setup.previous.log", ec);
        }
        log_.open(log_path_, std::ios::out | std::ios::trunc);
    }

    void log_line(const std::string& line) {
        if (log_.is_open()) {
            log_ << utc_now() << ' ' << line << '\n';
            log_.flush();
        }
    }

    // One line per event; progress every 5 %; an error's message after its first line as "  | " lines.
    void log_event(const Event& e) {
        if (!log_.is_open()) {
            return;
        }
        if (e.state == State::progress && e.total != 0) {
            const std::uint64_t step = e.done * 20 / e.total;
            const std::string key = e.unit + "/" + std::string(to_string(e.stage));
            const auto it = logged_step_.find(key);
            if (it != logged_step_.end() && it->second == step && e.done != e.total) {
                return;
            }
            logged_step_[key] = step;
        }
        const std::size_t newline = e.message.find('\n');
        std::string line = std::string(to_string(e.stage)) + " " + std::string(to_string(e.state));
        if (e.state == State::progress || e.total != 0) {
            line += " " + std::to_string(e.done) + "/" + std::to_string(e.total);
        }
        if (!e.unit.empty()) {
            line += " " + e.unit;
        }
        if (e.code) {
            line += " [" + std::string(core::to_string(*e.code)) + "]";
        }
        if (!e.message.empty()) {
            line += ": " + e.message.substr(0, newline);
        }
        log_line(line);
        if (newline != std::string::npos) {
            std::size_t start = newline + 1;
            while (start <= e.message.size()) {
                std::size_t end = e.message.find('\n', start);
                if (end == std::string::npos) {
                    end = e.message.size();
                }
                log_ << "  | " << e.message.substr(start, end - start) << '\n';
                start = end + 1;
            }
            log_.flush();
        }
    }

    // --- paths ---

    void resolve() {
        current_ = Stage::verify;
        work_ = o_.work_dir.empty() ? default_work_dir() : o_.work_dir;
        work_ = fs::absolute(work_);
        log_path_ = work_ / "logs" / "setup.log";
        open_log();
        log_line("setup " + std::string(version()) + ": iso " + utf8(o_.iso) + ", work " + utf8(work_) +
                 (o_.force ? ", forced" : ""));
        regions_ = o_.regions_dir;
        if (regions_.empty()) {
            regions_ = platform::find_data_dir(
                o_.executable_dir, {fs::path("data") / "regions", fs::path("share") / "dq8" / "regions"},
                ".");
        }
        if (regions_.empty()) {
            throw StageError(ErrorCode::tool_missing,
                             "the region profiles (data/regions) were not found near '" +
                                 utf8(o_.executable_dir) + "'; reinstall DQ8-Native");
        }
    }

    // --- stages ---

    void stage_verify() {
        start(Stage::verify, "checking " + utf8(o_.iso));
        const std::vector<RegionProfile> profiles = load_region_profiles(regions_);
        verified_.emplace(verify_image(
            o_.iso, profiles, [&](std::string_view what, std::uint64_t done, std::uint64_t total) {
                progress(done, total, {}, done == 0 ? "hashing " + std::string(what) : std::string());
                check_cancel();
            }));
        profile_id_ = verified_->profile.id;
        for (const std::string& w : verified_->warnings) {
            progress(0, 0, {}, "warning: " + w);
        }
        check_space();
        finish(State::done, profile_id_ + " (" + verified_->profile.title + "), boot file " +
                                verified_->boot.sha1.substr(0, 12) + " verified");
    }

    void check_space() {
        constexpr std::uint64_t estimate = std::uint64_t{2} << 30; // build trees, generated C++, modules
        constexpr std::uint64_t floor = std::uint64_t{256} << 20;
        std::uint64_t need = o_.required_free_bytes;
        if (need == 0) {
            const std::uint64_t used = directory_bytes(work_);
            need = used >= estimate - floor ? floor : estimate - used;
        }
        const fs::path where = existing_ancestor(work_);
        std::error_code ec;
        const fs::space_info space = fs::space(where, ec);
        if (ec) {
            progress(0, 0, {}, "warning: cannot read the free space of " + utf8(where) + ": " + ec.message());
            return;
        }
        if (space.available < need) {
            throw StageError(ErrorCode::insufficient_space, "the setup needs about " + gib(need) +
                                                                " free in " + utf8(where) + "; " +
                                                                gib(space.available) + " are free");
        }
    }

    void stage_extract() {
        start(Stage::extract, "copying the boot file, overlays and IOP modules");
        const RegionProfile& p = verified_->profile;
        std::uint64_t total = p.boot.size;
        bool known = p.boot.size != 0;
        for (const ProfileOverlay& ov : p.overlays) {
            total += ov.size;
            known = known && ov.size != 0;
        }
        for (const ProfileFile& f : p.files) {
            total += f.size;
            known = known && f.size != 0;
        }
        std::uint64_t finished = 0;
        std::uint64_t current_size = 0;
        std::string current;
        ExtractOptions eo;
        eo.force = o_.force;
        eo.progress = [&](std::string_view what, std::uint64_t done, std::uint64_t size) {
            if (what != current) {
                finished += current_size;
                current = std::string(what);
                current_size = size;
            }
            progress(known ? finished + done : 0, known ? total : 0, {}, done == 0 ? current : std::string());
            check_cancel();
        };
        const ExtractReport report = extract(*verified_, work_ / "baserom", eo);
        if (report.has_mismatch()) {
            std::string list;
            for (const ExtractedFile& f : report.files) {
                if (f.status == FileStatus::hash_mismatch) {
                    list += (list.empty() ? "" : ", ") + f.disc_path;
                }
            }
            throw StageError(ErrorCode::extract_hash_mismatch,
                             "files differ from the region profile: " + list);
        }
        const std::size_t fresh = report.count(FileStatus::extracted);
        finish(fresh == 0 ? State::skipped : State::done,
               std::to_string(report.files.size()) + " files in " + utf8(report.profile_dir) +
                   (fresh == 0 ? " (all up to date)" : " (" + std::to_string(fresh) + " written)"),
               known ? total : 0, known ? total : 0);
    }

    void stage_toolchain() {
        start(Stage::toolchain, "looking for the C++ compiler, cmake and ninja");
        toolchain::DetectOptions d;
        d.executable_dir = o_.executable_dir;
        d.bundled_root = o_.toolchain_root;
        toolchain_ = toolchain::detect(d);
#if DQ8_SETUP_HAVE_DOWNLOAD
        if (!toolchain_.bundled) {
            // Windows builds host and modules with the bundled llvm-mingw only (ADR-0018): fetch it, with the
            // user's consent, then look again.
            download_bundle(d);
            toolchain_ = toolchain::detect(d);
        }
#endif
        const auto describe = [](const char* name, const toolchain::Tool& t) {
            return std::string(name) + " " +
                   (t.found ? utf8(t.path) + (t.version.empty() ? "" : " " + t.version) : "missing");
        };
        progress(0, 0, {},
                 describe("c++", toolchain_.cxx) + "; " + describe("cmake", toolchain_.cmake) + "; " +
                     describe("ninja", toolchain_.ninja));
        const std::vector<toolchain::Problem> problems = toolchain::check(toolchain_);
        if (!problems.empty()) {
            std::string message;
            for (const toolchain::Problem& p : problems) {
                message += (message.empty() ? "" : "\n") + p.advice;
            }
            throw StageError(ErrorCode::toolchain_incomplete, message);
        }

        // The program's own parts the later stages need.
        ps2_recomp_ = locate_program(o_.ps2_recomp, o_.executable_dir, "ps2_recomp");
        if (ps2_recomp_.empty()) {
            throw StageError(ErrorCode::tool_missing, "the recompiler (ps2_recomp) was not found beside '" +
                                                          utf8(o_.executable_dir) +
                                                          "'; reinstall DQ8-Native");
        }
        module_template_ = o_.module_template_dir;
        if (module_template_.empty()) {
            module_template_ = platform::find_data_dir(
                o_.executable_dir,
                {fs::path("cmake") / "module", fs::path("share") / "dq8" / "cmake" / "module"},
                "CMakeLists.txt");
        }
        abi_include_ = o_.abi_include_dir;
        if (abi_include_.empty()) {
            abi_include_ = platform::find_data_dir(
                o_.executable_dir, {fs::path("include"), fs::path("share") / "dq8" / "include"},
                fs::path("dq8") / "abi" / "game_module.h");
        }
        if (module_template_.empty() || abi_include_.empty()) {
            throw StageError(ErrorCode::tool_missing,
                             "the module template (cmake/module) or the ABI headers were "
                             "not found; reinstall DQ8-Native");
        }
        module_args_ = module_cmake_args();
        if (o_.smoke_test) {
            game_ = locate_program(o_.game_executable, o_.executable_dir, "dq8-game");
            if (game_.empty()) {
                throw StageError(ErrorCode::tool_missing, "dq8-game was not found beside '" +
                                                              utf8(o_.executable_dir) +
                                                              "'; reinstall DQ8-Native");
            }
        }
        finish(State::done, (toolchain_.compiler_id.empty() ? std::string("c++") : toolchain_.compiler_id) +
                                " " + toolchain_.cxx.version + ", cmake " + toolchain_.cmake.version +
                                ", ninja " + toolchain_.ninja.version);
    }

#if DQ8_SETUP_HAVE_DOWNLOAD
    void download_bundle(const toolchain::DetectOptions& d) {
        const toolchain::DownloadPin& pin = toolchain::pinned_llvm_mingw();
        if (!o_.allow_toolchain_download) {
            throw StageError(
                ErrorCode::toolchain_incomplete,
                "the setup builds with its own toolchain (llvm-mingw " + pin.version + ", " +
                    std::to_string((pin.size + (1u << 20) - 1) >> 20) +
                    " MB to download); allow the download (dq8ctl setup --allow-download, or the "
                    "launcher's consent) and run the setup again");
        }
        toolchain::DownloadOptions dl;
        dl.consent = true;
        dl.bundled_root = d.bundled_root;
        dl.cmake = toolchain_.cmake.found ? toolchain_.cmake.path : std::filesystem::path("cmake");
        dl.cancel = o_.cancel;
        int last_phase = -1;
        std::uint64_t last_percent = ~std::uint64_t{0};
        dl.on_progress = [&](const toolchain::DownloadProgress& p) {
            static constexpr const char* names[] = {"download", "verify", "unpack"};
            const int phase = static_cast<int>(p.phase);
            const std::uint64_t percent = p.total != 0 ? p.done * 100 / p.total : p.done >> 20;
            if (phase == last_phase && percent == last_percent && p.done != p.total) {
                return;
            }
            last_phase = phase;
            last_percent = percent;
            progress(p.done, p.total, {}, names[phase]);
        };
        const toolchain::DownloadResult r = toolchain::download_toolchain(dl);
        progress(0, 0, {},
                 r.already_installed ? "llvm-mingw " + pin.version + " is installed"
                                     : "llvm-mingw " + pin.version + " installed in " + utf8(r.root));
    }
#endif

    // The module build's tool arguments: the detected compiler and ninja, or for the bundled llvm-mingw its
    // cmake toolchain file (cmake/toolchains/llvm-mingw.cmake, session E).
    std::vector<std::string> module_cmake_args() const {
        if (!toolchain_.bundled) {
            return toolchain::cmake_arguments(toolchain_);
        }
        const fs::path dir = platform::find_data_dir(
            o_.executable_dir,
            {fs::path("cmake") / "toolchains", fs::path("share") / "dq8" / "cmake" / "toolchains"},
            "llvm-mingw.cmake");
        if (dir.empty()) {
            throw StageError(ErrorCode::tool_missing,
                             "cmake/toolchains/llvm-mingw.cmake was not found; reinstall DQ8-Native");
        }
        const auto generic = [](const fs::path& p) {
            const std::u8string s = p.generic_u8string();
            return std::string(s.begin(), s.end());
        };
        std::vector<std::string> args{"-DCMAKE_TOOLCHAIN_FILE=" + generic(dir / "llvm-mingw.cmake"),
                                      "-DLLVM_MINGW_ROOT=" + generic(toolchain_.bundle_dir)};
        if (toolchain_.ninja.found) {
            args.push_back("-DCMAKE_MAKE_PROGRAM=" + generic(toolchain_.ninja.path));
        }
        return args;
    }

    void stage_maps() {
        start(Stage::maps, "looking for the function maps");
        units_ = recomp::code_units(*verified_);
        const bool contributor = !o_.maps_dir.empty();
        maps_ = o_.maps_dir;
        if (!contributor) {
            const fs::path root = platform::find_data_dir(
                o_.executable_dir, {fs::path("data") / "maps", fs::path("share") / "dq8" / "maps"},
                profile_id_);
            maps_ = root.empty() ? fs::path() : root / profile_id_;
        }
        const char* extension = contributor ? ".csv" : ".toml";
        std::string missing;
        for (const recomp::CodeUnit& u : units_) {
            std::error_code ec;
            if (maps_.empty() || !fs::is_regular_file(maps_ / (u.name + extension), ec)) {
                missing += (missing.empty() ? "" : ", ") + u.name;
            }
        }
        if (!missing.empty()) {
            throw StageError(ErrorCode::maps_missing,
                             contributor ? "the maps given with --maps (" + utf8(maps_) + ") lack " + missing
                             : maps_.empty()
                                 ? "no published function maps for " + profile_id_ + " (data/maps/" +
                                       profile_id_ + "); a contributor can pass --maps DIR"
                                 : "the published maps in '" + utf8(maps_) + "' lack " + missing +
                                       "; a contributor can pass --maps DIR");
        }
        finish(State::done, std::to_string(units_.size()) + (contributor ? " contributor" : " published") +
                                " maps in " + utf8(maps_));
    }

    recomp::PipelineOptions pipeline_options() {
        recomp::PipelineOptions po;
        po.work_dir = work_;
        po.maps_dir = maps_;
        po.ps2_recomp = ps2_recomp_;
        po.cmake = toolchain_.cmake.path;
        po.cmake_generator = "Ninja";
        po.module_source_dir = module_template_;
        po.abi_include_dir = abi_include_;
        po.extra_cmake_args = module_args_;
        po.jobs = jobs_;
        po.force = o_.force;
        po.cancel = o_.cancel;
        return po;
    }

    // Unit progress from the pipeline's on_progress: done counts the units before the current one.
    std::function<void(const std::string&, const std::string&)> unit_progress() {
        unit_index_ = 0;
        last_unit_.clear();
        return [this](const std::string& unit, const std::string& message) {
            if (unit != last_unit_) {
                if (!last_unit_.empty()) {
                    ++unit_index_;
                }
                last_unit_ = unit;
            }
            progress(unit_index_, units_.size(), unit, message);
        };
    }

    void stage_prepare() {
        start(Stage::prepare, "writing each unit's ELF and recompiler config");
        jobs_ = o_.jobs != 0 ? o_.jobs : toolchain::default_jobs();
        recomp::PipelineOptions po = pipeline_options();
        po.on_progress = unit_progress();
        source_ = recomp::disc_units(*verified_);
        prepared_ = recomp::prepare_units(source_, po);
        std::size_t written = 0;
        for (const recomp::PreparedUnit& p : prepared_.units) {
            written += (p.elf_written || p.config_written) ? 1 : 0;
            for (const std::string& w : p.warnings) {
                progress(unit_index_, units_.size(), p.unit.name, "warning: " + w);
            }
        }
        finish(written == 0 && !o_.force ? State::skipped : State::done,
               std::to_string(prepared_.units.size()) + " units" +
                   (written == 0 ? " (up to date)" : ", " + std::to_string(written) + " written"),
               prepared_.units.size(), prepared_.units.size());
    }

    void stage_generate() {
        start(Stage::generate, "recompiling each unit to C++ (ps2_recomp)");
        recomp::PipelineOptions po = pipeline_options();
        po.on_progress = unit_progress();
        generated_ = recomp::generate_units(prepared_, po);
        const std::size_t ran = static_cast<std::size_t>(
            std::count_if(generated_.units.begin(), generated_.units.end(),
                          [](const recomp::GeneratedUnit& g) { return !g.skipped; }));
        finish(ran == 0 ? State::skipped : State::done,
               std::to_string(generated_.units.size()) + " units with " + generated_.generator_id +
                   (ran == 0 ? " (up to date)" : ", " + std::to_string(ran) + " recompiled"),
               generated_.units.size(), generated_.units.size());
    }

    void stage_build() {
        start(Stage::build, "compiling the game modules with " + std::to_string(jobs_) + " jobs");
        recomp::PipelineOptions po = pipeline_options();
        std::string unit;
        std::string pending; // the tools' output up to the next newline
        po.on_progress = [&](const std::string& u, const std::string& message) {
            unit = u;
            progress(0, 0, u, message);
        };
        // ninja prints "[n/N] description" per step when its output is not a terminal.
        po.on_tool_output = [&](OutputStream, std::string_view chunk) {
            pending.append(chunk);
            std::size_t start_of_line = 0;
            for (std::size_t nl = pending.find('\n'); nl != std::string::npos;
                 nl = pending.find('\n', start_of_line)) {
                const std::string_view line(pending.data() + start_of_line, nl - start_of_line);
                start_of_line = nl + 1;
                if (line.size() < 5 || line.front() != '[') {
                    continue;
                }
                const std::size_t slash = line.find('/');
                const std::size_t close = line.find(']');
                if (slash == std::string_view::npos || close == std::string_view::npos || slash > close) {
                    continue;
                }
                std::uint64_t done = 0;
                std::uint64_t total = 0;
                bool ok = slash > 1 && close > slash + 1;
                for (std::size_t i = 1; ok && i < slash; ++i) {
                    ok = line[i] >= '0' && line[i] <= '9';
                    done = done * 10 + static_cast<std::uint64_t>(line[i] - '0');
                }
                for (std::size_t i = slash + 1; ok && i < close; ++i) {
                    ok = line[i] >= '0' && line[i] <= '9';
                    total = total * 10 + static_cast<std::uint64_t>(line[i] - '0');
                }
                if (ok && total != 0) {
                    progress(done, total, unit, {});
                }
            }
            pending.erase(0, start_of_line);
        };
        built_ = recomp::build_units(prepared_, generated_, po);
        modules_dir_ = built_.modules_dir;
        // Up to date when every module came out as it was (cmake/module relinks on every build, so ninja's
        // "no work to do" alone would never say so).
        std::size_t updated = 0;
        for (const recomp::BuiltUnit& b : built_.units) {
            updated += b.module_updated ? 1 : 0;
        }
        finish(updated == 0 && !o_.force ? State::skipped : State::done,
               std::to_string(built_.units.size()) + " modules in " + utf8(modules_dir_) +
                   (updated == 0 ? " (up to date)" : ", " + std::to_string(updated) + " updated"));
    }

    void stage_smoke() {
        start(Stage::smoke, "starting the game headless");
        if (!o_.smoke_test) {
            finish(State::skipped, "the smoke test is turned off");
            return;
        }
        const fs::path manifest = built_.manifest;
        manifest_sha1_ = platform::sha1_file(manifest);
        const std::string game_sha1 = platform::sha1_file(game_);
        const fs::path stamp = work_ / "stamps" / "smoke.toml";
        std::error_code ec;
        if (!o_.force && fs::exists(stamp, ec)) {
            try {
                const toml::value t = toml::parse(stamp);
                if (toml::find_or<std::string>(t, "manifest_sha1", std::string()) == manifest_sha1_ &&
                    toml::find_or<std::string>(t, "game_sha1", std::string()) == game_sha1) {
                    finish(State::skipped, "these modules passed with this dq8-game on " +
                                               toml::find_or<std::string>(t, "passed_at", std::string("?")));
                    return;
                }
            } catch (const std::exception&) {
                // An unreadable stamp: run the test again.
            }
        }

        const fs::path input = o_.smoke_input.empty() ? shipped_smoke_input() : o_.smoke_input;
        ProcessOptions p;
        p.program = game_;
        p.args = {"--iso",        utf8(fs::absolute(o_.iso)),
                  "--module-dir", utf8(modules_dir_),
                  "--regions",    utf8(fs::absolute(regions_)),
                  "--headless",   "--movies",
                  "skip",         "--json"};
        std::string what;
        if (!input.empty()) {
            // The profile's own input gets past the first-run screens; the title is the goal.
            p.args.insert(p.args.end(),
                          {"--input-script", utf8(input), "--until", "title", "--vblanks", "3000"});
            what = "booting to the title screen";
        } else if (const std::string& overlay = verified_->profile.title_screen.overlay; !overlay.empty()) {
            // No input for this profile: the title's overlay becoming resident still proves the modules and
            // the overlay residency work.
            p.args.insert(p.args.end(), {"--until", "overlay:" + overlay, "--vblanks", "3000"});
            what = "running until " + overlay + " is resident";
        } else {
            p.args.insert(p.args.end(), {"--vblanks", "600"});
            what = "running 600 vblanks";
        }
        p.timeout = o_.smoke_timeout;
        p.cancel = o_.cancel;
        progress(0, 0, {}, what);
        const ProcessResult r = run_process(p);
        {
            std::ofstream log(work_ / "logs" / "smoke.log", std::ios::trunc);
            log << "$ " << describe_command(p.program, p.args) << "\n"
                << r.out << "\n--- stderr ---\n"
                << r.err << "\n--- exit status " << r.exit_code << ", " << r.elapsed.count() << " ms ---\n";
        }
        if (r.cancelled) {
            throw StageError(ErrorCode::setup_cancelled, "the setup was cancelled");
        }
        if (!r.ok()) {
            const std::string why = r.timed_out
                                        ? "ran past " + std::to_string(o_.smoke_timeout.count()) + " s"
                                        : "exited with status " + std::to_string(r.exit_code);
            throw StageError(ErrorCode::smoke_failed, "dq8-game " + why + " while " + what + " (log: " +
                                                          utf8(work_ / "logs" / "smoke.log") + ")\n" +
                                                          tail_lines(r.err.empty() ? r.out : r.err, 8));
        }
        toml::ordered_value t(toml::ordered_table{});
        t["manifest_sha1"] = manifest_sha1_;
        t["game_sha1"] = game_sha1;
        t["passed_at"] = utc_now();
        t["seconds"] = static_cast<double>(r.elapsed.count()) / 1000.0;
        platform::write_file_atomic(stamp,
                                    "# The setup's smoke test passed for these modules.\n" + toml::format(t));
        finish(State::done, what + ": passed in " + std::to_string(r.elapsed.count() / 1000) + " s");
    }

    // data/smoke/smoke.toml names the profile's input script ([profiles] <id> = "<file>"); empty without one.
    fs::path shipped_smoke_input() const {
        const fs::path dir = platform::find_data_dir(
            o_.executable_dir, {fs::path("data") / "smoke", fs::path("share") / "dq8" / "smoke"},
            "smoke.toml");
        if (dir.empty()) {
            return {};
        }
        const toml::value index = toml::parse(dir / "smoke.toml");
        const toml::value profiles = toml::find_or(index, "profiles", toml::value(toml::table{}));
        if (!profiles.is_table() || !profiles.contains(profile_id_) ||
            !profiles.at(profile_id_).is_string()) {
            return {};
        }
        return dir / profiles.at(profile_id_).as_string();
    }

    void stage_register() {
        start(Stage::register_install, "recording the build for Play");
        if (manifest_sha1_.empty()) {
            manifest_sha1_ = platform::sha1_file(built_.manifest);
        }
        const fs::path file = o_.registry_file.empty() ? registry::default_file() : o_.registry_file;
        registry::Registry reg = registry::Registry::load(file);
        registry::Install install;
        install.profile_id = profile_id_;
        install.iso = fs::absolute(o_.iso);
        install.module_dir = modules_dir_;
        install.manifest_sha1 = manifest_sha1_;
        install.built_at = utc_now();
        install.host_version = version();
        if (const registry::Install* old = reg.find(profile_id_);
            old != nullptr && !o_.force && old->iso == install.iso && old->module_dir == install.module_dir &&
            old->manifest_sha1 == install.manifest_sha1 && old->host_version == install.host_version) {
            finish(State::skipped, profile_id_ + " is already registered in " + utf8(file));
            return;
        }
        reg.upsert(std::move(install));
        reg.save(file);
        finish(State::done, profile_id_ + " registered in " + utf8(file));
    }

    const Options& o_;
    fs::path work_;
    fs::path log_path_;
    std::ofstream log_;
    fs::path regions_;
    Stage current_ = Stage::verify;
    bool started_ = false;
    std::map<std::string, int, std::less<>> last_percent_;
    std::map<std::string, std::uint64_t, std::less<>> logged_step_;

    std::optional<VerifiedImage> verified_;
    std::string profile_id_;
    toolchain::Toolchain toolchain_;
    std::vector<std::string> module_args_;
    fs::path ps2_recomp_;
    fs::path module_template_;
    fs::path abi_include_;
    fs::path game_;
    fs::path maps_;
    std::vector<recomp::CodeUnit> units_;
    unsigned jobs_ = 0;
    recomp::UnitSource source_;
    recomp::PrepareReport prepared_;
    recomp::GenerateReport generated_;
    recomp::BuildReport built_;
    fs::path modules_dir_;
    std::string manifest_sha1_;
    std::size_t unit_index_ = 0;
    std::string last_unit_;
};

} // namespace

std::string_view to_string(Stage stage) noexcept {
    switch (stage) {
    case Stage::verify:
        return "verify";
    case Stage::extract:
        return "extract";
    case Stage::toolchain:
        return "toolchain";
    case Stage::maps:
        return "maps";
    case Stage::prepare:
        return "prepare";
    case Stage::generate:
        return "generate";
    case Stage::build:
        return "build";
    case Stage::smoke:
        return "smoke";
    case Stage::register_install:
        return "register";
    }
    return "unknown";
}

std::string_view to_string(State state) noexcept {
    switch (state) {
    case State::start:
        return "start";
    case State::progress:
        return "progress";
    case State::done:
        return "done";
    case State::skipped:
        return "skipped";
    case State::error:
        return "error";
    }
    return "unknown";
}

std::string to_json_line(const Event& event) {
    std::string out = R"({"ev":"stage","stage":)";
    append_json_string(out, to_string(event.stage));
    out += R"(,"state":)";
    append_json_string(out, to_string(event.state));
    out += R"(,"done":)" + std::to_string(event.done);
    out += R"(,"total":)" + std::to_string(event.total);
    if (!event.unit.empty()) {
        out += R"(,"unit":)";
        append_json_string(out, event.unit);
    }
    if (!event.message.empty()) {
        out += R"(,"message":)";
        append_json_string(out, event.message);
    }
    if (event.code) {
        out += R"(,"code":)";
        append_json_string(out, core::to_string(*event.code));
    }
    out += '}';
    return out;
}

fs::path default_work_dir() {
    return platform::user_data_dir() / "build";
}

Result run(const Options& options) {
    Runner runner(options);
    return runner.run();
}

} // namespace dq8::core::setup
