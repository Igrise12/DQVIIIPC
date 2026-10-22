// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// dq8ctl setup, run and diagnose (M5): the first-run setup over setup::run(), starting dq8-game from the
// build registry, and the diagnostics report.
#include "cli.h"
#include "dq8/core/diagnostics.h"
#include "dq8/core/error_text.h"
#include "dq8/core/process.h"
#include "dq8/core/registry.h"
#include "dq8/core/settings.h"
#include "dq8/core/setup.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>

namespace dq8::cli {

namespace {

namespace setup = dq8::core::setup;

// Ctrl-C (and SIGTERM) cancel the setup: the process runner stops the tool tree, the stage ends with
// setup-cancelled, and a later run resumes. A lock-free atomic store is signal-safe.
std::atomic<bool> cancel_requested{false};

void on_interrupt(int) {
    cancel_requested.store(true);
}

std::optional<std::string> value(const Args& args, const char* key) {
    const auto it = args.values.find(key);
    return it == args.values.end() ? std::nullopt : std::optional<std::string>(it->second);
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

// --ps2-recomp, else the recompiler this build produced; empty lets the setup look beside dq8ctl and on PATH.
fs::path ps2_recomp(const Args& args) {
    if (const auto v = value(args, "ps2-recomp")) {
        return *v;
    }
#ifdef DQ8_DEFAULT_PS2_RECOMP
    std::error_code ec;
    if (fs::is_regular_file(DQ8_DEFAULT_PS2_RECOMP, ec)) {
        return DQ8_DEFAULT_PS2_RECOMP;
    }
#endif
    return {};
}

// --game, else the dq8-game this build produced, else dq8-game beside dq8ctl or on PATH.
fs::path game(const Args& args, const fs::path& exe_dir) {
    if (const auto v = value(args, "game")) {
        return *v;
    }
#ifdef DQ8_DEFAULT_GAME
    std::error_code ec;
    if (fs::is_regular_file(DQ8_DEFAULT_GAME, ec)) {
        return DQ8_DEFAULT_GAME;
    }
#endif
#if defined(_WIN32)
    const fs::path beside = exe_dir / "dq8-game.exe";
#else
    const fs::path beside = exe_dir / "dq8-game";
#endif
    if (const auto found = dq8::core::find_program(beside)) {
        return *found;
    }
    return "dq8-game";
}

fs::path settings_file(const Args& args, const fs::path& exe_dir) {
    if (const auto v = value(args, "config")) {
        return *v;
    }
    return dq8::core::settings::default_user_file(exe_dir);
}

// A person's view of one event: stage starts and ends, messages, and counted progress every 10 %.
class HumanProgress {
  public:
    explicit HumanProgress(std::FILE* out) : out_(out) {}
    void operator()(const setup::Event& e) {
        const std::string stage(setup::to_string(e.stage));
        switch (e.state) {
        case setup::State::start:
            std::fprintf(out_, "%-9s ...%s%s\n", stage.c_str(), e.message.empty() ? "" : " ",
                         e.message.c_str());
            tenth_ = -1;
            break;
        case setup::State::progress: {
            const std::string unit = e.unit.empty() ? std::string() : " [" + e.unit + "]";
            if (!e.message.empty()) {
                std::fprintf(out_, "%-9s %s%s\n", "", e.message.c_str(), unit.c_str());
            } else if (e.total != 0) {
                const int tenth = static_cast<int>(e.done * 10 / e.total);
                if (tenth != tenth_ || e.unit != unit_) {
                    tenth_ = tenth;
                    unit_ = e.unit;
                    std::fprintf(out_, "%-9s %llu/%llu%s\n", "", static_cast<unsigned long long>(e.done),
                                 static_cast<unsigned long long>(e.total), unit.c_str());
                }
            }
            break;
        }
        case setup::State::done:
        case setup::State::skipped:
            std::fprintf(out_, "%-9s %s: %s\n", stage.c_str(), std::string(setup::to_string(e.state)).c_str(),
                         e.message.c_str());
            break;
        case setup::State::error:
            std::fprintf(out_, "%-9s error [%s]: %s\n", stage.c_str(),
                         e.code ? std::string(dq8::core::to_string(*e.code)).c_str() : "?",
                         e.message.c_str());
            break;
        }
        std::fflush(out_);
    }

  private:
    std::FILE* out_;
    int tenth_ = -1;
    std::string unit_;
};

} // namespace

int run_setup(const Args& args, const char* argv0) {
    if (args.positional.size() != 1) {
        std::fputs("dq8ctl setup: expected one <iso>\n", stderr);
        return usage(stderr);
    }
    const bool json = args.flags.count("json") != 0;
    const fs::path exe_dir = executable_dir(argv0);
    setup::Options o;
    o.iso = args.positional[0];
    o.executable_dir = exe_dir;
    if (const auto v = value(args, "work")) {
        o.work_dir = *v;
    }
    if (const auto v = value(args, "maps")) {
        o.maps_dir = *v;
    }
    if (const auto v = value(args, "regions")) {
        o.regions_dir = *v;
    }
    if (const auto v = value(args, "jobs")) {
        o.jobs = parse_jobs(*v);
    }
    if (const auto v = value(args, "registry")) {
        o.registry_file = *v;
    }
    if (const auto v = value(args, "smoke-input")) {
        o.smoke_input = *v;
    }
    o.force = args.flags.count("force") != 0;
    o.smoke_test = args.flags.count("no-smoke") == 0;
    o.ps2_recomp = ps2_recomp(args);
    o.game_executable = game(args, exe_dir);

    // --events: JSON lines on stdout ("-") or into a file; otherwise readable lines (on stderr with --json).
    std::unique_ptr<std::ofstream> events_file;
    const std::optional<std::string> events = value(args, "events");
    if (events && *events != "-") {
        events_file = std::make_unique<std::ofstream>(fs::path(*events), std::ios::trunc);
        if (!*events_file) {
            throw Error(ErrorCode::io_write, "cannot create '" + *events + "'");
        }
    }
    HumanProgress human(json ? stderr : stdout);
    o.on_event = [&](const setup::Event& e) {
        if (events && *events == "-") {
            std::printf("%s\n", setup::to_json_line(e).c_str());
            std::fflush(stdout);
        } else {
            if (events_file) {
                *events_file << setup::to_json_line(e) << '\n';
                events_file->flush();
            }
            human(e);
        }
    };
    o.cancel = &cancel_requested;
    std::signal(SIGINT, on_interrupt);
    std::signal(SIGTERM, on_interrupt);

    const setup::Result r = setup::run(o);
    if (json) {
        Json j;
        j.begin_object();
        j.member("ok", r.ok);
        if (!r.ok && r.code) {
            j.key("error");
            j.begin_object();
            j.member("code", dq8::core::to_string(*r.code));
            j.member("message", r.message);
            j.member("action", dq8::core::error_text(*r.code).action);
            j.end_object();
        }
        j.member("profile", r.profile_id);
        j.member("module_dir", path_text(r.module_dir));
        j.member("work", path_text(r.work_dir));
        j.member("log", path_text(r.log_file));
        j.end_object();
        std::printf("%s\n", j.str().c_str());
    } else if (!(events && *events == "-")) {
        if (r.ok) {
            std::printf("ready: %s (log %s)\n", r.message.c_str(), path_text(r.log_file).c_str());
        } else if (r.code) {
            const dq8::core::ErrorText text = dq8::core::error_text(*r.code);
            std::fprintf(stderr, "dq8ctl setup: %s %s [%s]\n  log: %s\n", std::string(text.message).c_str(),
                         std::string(text.action).c_str(), std::string(dq8::core::to_string(*r.code)).c_str(),
                         path_text(r.log_file).c_str());
        }
    }
    if (r.ok) {
        return exit_ok;
    }
    return r.code && dq8::core::is_mismatch(*r.code) ? exit_mismatch : exit_error;
}

int run_game(const Args& args, const char* argv0) {
    if (!args.positional.empty()) {
        std::fputs("dq8ctl run: unexpected argument; dq8-game options go after --\n", stderr);
        return usage(stderr);
    }
    const fs::path exe_dir = executable_dir(argv0);
    const fs::path registry_file =
        value(args, "registry") ? fs::path(*value(args, "registry")) : dq8::core::registry::default_file();
    const dq8::core::registry::Registry registry = dq8::core::registry::Registry::load(registry_file);
    const dq8::core::registry::Install* install = nullptr;
    if (const auto profile = value(args, "profile")) {
        install = registry.find(*profile);
        if (install == nullptr) {
            throw Error(ErrorCode::invalid_argument,
                        "no build of " + *profile + " is registered in " + path_text(registry_file));
        }
    } else if (!registry.installs().empty()) {
        install = &registry.installs().front();
    } else {
        throw Error(ErrorCode::invalid_argument, "no game is set up (" + path_text(registry_file) +
                                                     " lists none); run `dq8ctl setup <iso>` first");
    }

    dq8::core::ProcessOptions p;
    p.program = game(args, exe_dir);
    p.args = {"--iso",        path_text(install->iso),
              "--module-dir", path_text(install->module_dir),
              "--config",     path_text(settings_file(args, exe_dir))};
    bool display_given = false;
    for (const std::string& a : args.passthrough) {
        display_given = display_given || a == "--headless" || a == "--window";
    }
    if (!display_given) {
        p.args.push_back("--window");
    }
    p.args.insert(p.args.end(), args.passthrough.begin(), args.passthrough.end());
    p.on_output = [](dq8::core::OutputStream stream, std::string_view chunk) {
        std::FILE* out = stream == dq8::core::OutputStream::out ? stdout : stderr;
        std::fwrite(chunk.data(), 1, chunk.size(), out);
        std::fflush(out);
    };
    p.capture_limit = 1u << 20;
    // The game leads its own process group, so Ctrl-C reaches only dq8ctl: pass it on as a stop.
    p.cancel = &cancel_requested;
    std::signal(SIGINT, on_interrupt);
    std::signal(SIGTERM, on_interrupt);
    std::fprintf(stderr, "dq8ctl run: %s\n", dq8::core::describe_command(p.program, p.args).c_str());
    const dq8::core::ProcessResult r = dq8::core::run_process(p);
    if (r.signal != 0) {
        std::fprintf(stderr, "dq8ctl run: dq8-game ended by signal %d\n", r.signal);
        return exit_error;
    }
    return r.exit_code;
}

int run_diagnose(const Args& args, const char* argv0) {
    if (!args.positional.empty()) {
        std::fputs("dq8ctl diagnose: unexpected argument\n", stderr);
        return usage(stderr);
    }
    dq8::core::diagnostics::Options o;
    o.executable_dir = executable_dir(argv0);
    if (const auto v = value(args, "config")) {
        o.settings_file = *v;
    }
    if (const auto v = value(args, "registry")) {
        o.registry_file = *v;
    }
    if (const auto v = value(args, "work")) {
        o.work_dir = *v;
    }
    if (const auto out = value(args, "out")) {
        dq8::core::diagnostics::write_report(o, *out);
        std::printf("wrote %s\n", out->c_str());
    } else {
        std::fputs(dq8::core::diagnostics::report(o).c_str(), stdout);
    }
    return exit_ok;
}

} // namespace dq8::cli
