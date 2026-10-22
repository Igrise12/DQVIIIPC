// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/ee/overlay_manager.h"
#include "runtime/machine/machine_services.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// dq8-game's command line (src/host/README.md has the reference). Parsing only: no file is opened and the
// region profile is not read here, so `--until` and `--stop-at-pc` are checked against the disc later.
namespace dq8::host {

namespace fs = std::filesystem;

// Exit status.
inline constexpr int exit_ok = 0;            // the run ended as asked (see README)
inline constexpr int exit_guest_stop = 1;    // any other guest stop
inline constexpr int exit_error = 2;         // usage, I/O, disc verification, module validation
inline constexpr int exit_backend = 3;       // the GS backend or the platform (window) failed to start
inline constexpr int exit_until_not_met = 4; // --vblanks ran out before the --until condition held

class UsageError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

enum class Display { headless, window };
enum class GsKind { null, trace, parallel };

// --stop-at-rpc SID (the bind of server SID) or SID:FNO (a call of function FNO on it).
struct RpcStop {
    std::uint32_t sid = 0;
    std::optional<std::uint32_t> fno;
    bool operator==(const RpcStop&) const = default;
};

struct AtVblank {
    std::uint64_t vblank = 0;
    fs::path path;
};

struct Options {
    // What to run.
    std::optional<fs::path> iso;
    std::optional<fs::path> elf;
    std::optional<fs::path> regions;
    std::optional<fs::path> module_dir;
    std::optional<rt::OverlayWindowSpec> overlay_window; // --overlay-window / M2's --window BASE:END
    std::optional<std::uint32_t> resident;
    std::optional<fs::path> disc; // --disc: developer mode's disc image for the IOP (not verified)

    // M2 run options.
    bool stop_at_first_syscall = false;
    std::uint64_t max_dispatches = 0;
    bool trace_dispatch = false;
    bool trap_mmio = false;
    bool json = false;

    // M3.
    Display display = Display::headless;
    GsKind gs = GsKind::null;
    std::optional<std::uint64_t> vblanks;
    std::optional<std::string> until;
    std::vector<AtVblank> dump_frames;
    std::optional<fs::path> gs_trace;
    std::optional<fs::path> input_script;
    std::optional<fs::path> record_input;
    std::optional<double> speed; // nullopt: as fast as possible ("max")
    std::uint32_t ssaa = 1;
    std::string movies = "auto";
    std::optional<std::string> events; // a file, or "-" for stdout
    std::uint32_t trace = 0;           // rt::trace_bit() mask (MachineConfig::trace_mask)
    bool trace_events = false;         // also write "trace" events into --events
    bool strict_kernel = false;
    bool verify_overlays = false;
    std::vector<std::uint32_t> stop_at_pc;
    std::vector<RpcStop> stop_at_rpc;
    std::vector<AtVblank> dump_ram_at;
    // M4 (Docs/milestones/M4-playable.md, "Contracts"). Options whose subsystem has not arrived yet are
    // refused at start-up with the session that brings them (run_machine.cpp, unwired_m4_option).
    std::string audio;                       // "null" or "sdl"; set by parsing: sdl with --window, else null
    std::optional<fs::path> audio_dump;      // a WAV file of everything the SPU2 mixed
    std::uint32_t audio_latency_ms = 64;     // the SDL sink's target buffer
    std::optional<fs::path> movie_decoder;   // the dq8-movie library to load
    std::optional<std::string> save_dir;     // a directory, or "none"; unset: the platform default
    std::string memory_card = "unformatted"; // port 0: unformatted, formatted or none
    std::string memory_card2 = "none";       // port 1
    std::optional<std::string> rtc;          // YYYY-MM-DDTHH:MM:SS or "host"; unset: the fixed epoch
    bool rumble = true;
    std::string vu0_registers = "per-thread"; // or "shared"
    std::optional<fs::path> dump_dir;         // where `mark NAME` script lines dump the next frame
    std::vector<AtVblank> dump_iop_ram_at;
    std::vector<AtVblank> dump_spu2_at;
    // M5 (Docs/milestones/M5-launcher.md, "Settings to dq8-game"): settings (ADR-0006; settings_wiring.h) and
    // the launcher's status channel (status_channel.h). With --config or --set, every setting whose flag is
    // not on the command line applies: explicit flags win over --set, which wins over --config.
    std::optional<fs::path> config;    // a settings file (the user file, with its [profile.<id>] tables)
    std::vector<std::string> sets;     // --set KEY=VALUE, over the file
    std::optional<std::string> status; // a file, or "-" for stdout: started, first_frame, fps, warning, exit
    std::vector<std::string> given;    // the options on the command line, by name without "--", in order

    // True when option `name` (without "--") was on the command line.
    bool was_given(std::string_view name) const noexcept;

    bool help = false;
    bool version = false;

    // True when any option needs the M3 machine (everything beyond M2's single-thread run).
    bool uses_machine = false;
};

// Throws UsageError with a one-line message.
Options parse_options(int argc, const char* const* argv);

// The usage text; returns the exit status to use (0 on stdout, exit_error otherwise).
int usage(std::FILE* out);

// "0x1C" or "28" -> 28. Throws UsageError naming `what`.
std::uint64_t parse_number(std::string_view text, std::string_view what);
std::uint32_t parse_u32(std::string_view text, std::string_view what);

} // namespace dq8::host
