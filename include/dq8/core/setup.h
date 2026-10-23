// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/error.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// The first-run setup (architecture.md §3): from the user's disc image to an installed, playable build, as
// one resumable operation with structured progress. It runs the existing stages (intake, recompile, module
// build) and adds the toolchain check, the function maps, a smoke test and the build registry. The launcher
// calls run() on a worker thread; `dq8ctl setup --events -` prints the same events as JSON lines. Stages are
// idempotent: a run after a cancel or a failure skips what is already up to date (verify, toolchain and maps
// are quick checks that always run).
//
// The work directory (default user_data_dir()/build) holds baserom/ (the extracted files), the recompile
// pipeline's layout (recomp::WorkLayout: units/, generated/, build/, modules/<profile>/, logs/, stamps/) and
// logs/setup.log, a text log of every run: one line per event (progress thinned to every 5 %), and for an
// error its full message on following lines that start with "  | " (those may quote tool output, so the
// diagnostics export leaves them out).
namespace dq8::core::setup {

enum class Stage {
    verify,           // the disc image against the region profiles
    extract,          // the boot file, overlays and modules into the work directory
    toolchain,        // the compiler, cmake and ninja (on Windows the bundled toolchain, ADR-0018)
    maps,             // the published function maps for each unit (ADR-0017)
    prepare,          // unit ELFs and recompiler configs
    generate,         // ps2_recomp
    build,            // the game modules
    smoke,            // dq8-game headless to the title screen
    register_install, // the build registry entry the launcher's Play uses
};

// Every stage, in the order run() reports them.
inline constexpr std::array<Stage, 9> stages{Stage::verify, Stage::extract, Stage::toolchain,
                                             Stage::maps,   Stage::prepare, Stage::generate,
                                             Stage::build,  Stage::smoke,   Stage::register_install};

enum class State { start, progress, done, skipped, error };

struct Event {
    Stage stage = Stage::verify;
    State state = State::start;
    std::uint64_t done = 0;        // progress: work done so far (bytes, files, compile steps)
    std::uint64_t total = 0;       // 0 when unknown
    std::string unit;              // the code unit, when the stage works per unit
    std::string message;           // for people
    std::optional<ErrorCode> code; // State::error
};

std::string_view to_string(Stage stage) noexcept; // "verify", ..., "register"
std::string_view to_string(State state) noexcept; // "start", "progress", "done", "skipped", "error"

// The wire form, one line without the newline:
// {"ev":"stage","stage":"build","state":"progress","done":120,"total":845,"unit":"main","message":"...","code":"..."}
// "unit", "message" and "code" appear only when set.
std::string to_json_line(const Event& event);

using EventSink = std::function<void(const Event&)>;

struct Options {
    std::filesystem::path iso;
    std::filesystem::path work_dir; // default: default_work_dir()
    std::filesystem::path maps_dir; // empty: the published maps (data/maps/<profile>); else contributor maps
    std::filesystem::path regions_dir;    // empty: found next to the program
    std::filesystem::path executable_dir; // where dq8ctl, ps2_recomp and dq8-game are
    unsigned jobs = 0;                    // 0: toolchain::default_jobs()
    bool force = false;                   // redo stages even when up to date
    bool smoke_test = true;
    const std::atomic<bool>* cancel = nullptr; // set to stop at the next safe point (setup_cancelled)
    EventSink on_event;

    // Where the setup's programs and data are; each empty one is found from executable_dir: beside it (an
    // install), else in the source tree above it (a build tree), else on PATH for the programs.
    std::filesystem::path ps2_recomp;          // the recompiler
    std::filesystem::path game_executable;     // dq8-game, for the smoke test
    std::filesystem::path module_template_dir; // cmake/module
    std::filesystem::path abi_include_dir;     // the directory holding dq8/abi/
    std::filesystem::path smoke_input;     // the smoke test's input script; empty: data/smoke/smoke.toml's
    std::filesystem::path toolchain_root;  // toolchain::DetectOptions::bundled_root
    std::filesystem::path registry_file;   // empty: registry::default_file()
    std::uint64_t required_free_bytes = 0; // 0: the estimate (2 GiB less what the work directory holds)
    std::chrono::seconds smoke_timeout{900};
    // The user's consent to download the bundled toolchain (Windows, ADR-0018; the launcher's consent dialog,
    // `dq8ctl setup --allow-download`). Without it a missing bundle is toolchain-incomplete with its size.
    bool allow_toolchain_download = false;
};

struct Result {
    bool ok = false;
    std::optional<ErrorCode> code;
    std::string message;
    std::string profile_id;
    std::filesystem::path module_dir;
    std::filesystem::path work_dir;
    std::filesystem::path log_file; // logs/setup.log in the work directory
};

// platform::user_data_dir() / "build". Throws Error(io_open) without a home directory.
std::filesystem::path default_work_dir();

// Never throws for setup failures: they end in a Result with the code and an error Event.
Result run(const Options& options);

} // namespace dq8::core::setup
