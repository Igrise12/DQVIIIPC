// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The M3 run of dq8-game (architecture §12, ADR-0005, ADR-0007): the machine thread runs Machine::run in
// one-vblank slices with the limiter between them; the GS pipeline's render thread drives the backend; the
// main thread runs the platform loop with --window (events, live input, presentation) or waits.
#include "dq8/core/error.h"
#include "dq8/core/hash.h"
#include "dq8/core/iso9660.h"
#include "game/iop_modules.h"
#include "game/mode_watcher.h"
#include "game/movie_timelines.h"
#include "game/movies.h"
#include "game/overlay_residency.h"
#include "game/title_watcher.h"
#include "game/unit_specs.h"
#include "game/until.h"
#include "host/event_log.h"
#include "host/limiter.h"
#include "host/m4_wiring.h"
#include "host/run.h"
#include "host/session.h"
#include "host/settings_wiring.h"
#include "host/status_channel.h"
#include "runtime/ee/context.h"
#include "runtime/ee/ee_runtime.h"
#include "runtime/ee/game_module.h"
#include "runtime/gs/gs_backends.h"
#include "runtime/gs/gs_pipeline.h"
#include "runtime/io/disc_source.h"
#include "runtime/io/input_live.h"
#include "runtime/io/input_record.h"
#include "runtime/io/input_script.h"
#include "runtime/iop/iop.h"
#include "runtime/machine/ee_devices.h"
#include "runtime/machine/machine.h"
#include "runtime/vu/vu_units.h"
// The paraLLEl-GS backend (session D) when the tree has it.
#if __has_include("runtime/gs/gs_parallel_backend.h")
#include "runtime/gs/gs_parallel_backend.h"
#define DQ8_HOST_HAVE_GS_PARALLEL 1
#endif
#ifdef DQ8_HAVE_WINDOW
#include "runtime/platform/platform.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace dq8::host {

namespace {

using rt::hex32;

// Developer mode without --disc: a drive with no disc in it.
class NoDisc final : public rt::IDiscSource {
  public:
    std::uint32_t sector_count() const override { return 0; }
    std::optional<rt::DiscFile> find(std::string_view) const override { return std::nullopt; }
    void read_sectors(std::uint32_t lsn, std::uint32_t, std::span<std::uint8_t>) const override {
        throw core::Error(core::ErrorCode::io_read, "read of sector " + std::to_string(lsn) +
                                                        " with no disc (developer mode without --disc)");
    }
};

std::string_view gs_name(GsKind k) noexcept {
    switch (k) {
    case GsKind::null:
        return "null";
    case GsKind::trace:
        return "trace";
    case GsKind::parallel:
        return "parallel";
    }
    return "?";
}

std::unique_ptr<gs::IGsBackend> make_backend(const Options& o, const char* argv0) {
    try {
        switch (o.gs) {
        case GsKind::null:
            return gs::make_null_gs_backend();
        case GsKind::trace:
            return gs::make_trace_gs_backend(o.gs_trace.value_or(fs::path()));
        case GsKind::parallel: {
#ifdef DQ8_HOST_HAVE_GS_PARALLEL
            gs::ParallelGsConfig
                config; // library empty: gs::default_parallel_gs_library() (the environment,
                        // next to the executable, ../lib for an installed tree, the build path)
            (void)argv0;
            config.super_sampling = o.ssaa;
            return gs::make_parallel_gs_backend(config);
#else
            (void)argv0;
            throw BackendError("--gs parallel: this build has no paraLLEl-GS backend");
#endif
        }
        }
    } catch (const BackendError&) {
        throw;
    } catch (const std::exception& e) {
        throw BackendError("the " + std::string(gs_name(o.gs)) + " GS backend failed to start: " + e.what());
    }
    throw BackendError("unknown GS backend");
}

rt::VideoTiming video_for(const Session& s) {
    const core::RegionProfile* p = s.profile();
    return p != nullptr && p->video_mode == "PAL" ? rt::VideoTiming::pal() : rt::VideoTiming::ntsc();
}

double seconds_of(rt::Cycles cycles) {
    return static_cast<double>(cycles) / static_cast<double>(rt::ee_clock_hz);
}

std::string ram_sha1(const rt::GuestMemory& memory) {
    constexpr std::size_t ram_bytes = std::size_t{32} << 20;
    return core::to_hex(core::Sha1::of(std::span<const std::uint8_t>(memory.rdram(), ram_bytes)));
}

void write_ram(const rt::GuestMemory& memory, const fs::path& path) {
    constexpr std::size_t ram_bytes = std::size_t{32} << 20;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(memory.rdram()), static_cast<std::streamsize>(ram_bytes));
    if (!out) {
        throw core::Error(core::ErrorCode::io_write, "cannot write the RAM dump " + path.string());
    }
}

// Overrides for --stop-at-pc: on the main unit when it has an entry there, else on every overlay unit with
// one.
std::vector<rt::OverrideSpec> stop_at_pc_overrides(const Options& o, const Session& s) {
    std::vector<rt::OverrideSpec> out;
    for (const std::uint32_t pc : o.stop_at_pc) {
        std::vector<std::string> units;
        if (s.dispatcher->main_table().is_function_entry(pc)) {
            units.push_back(s.dispatcher->main_name());
        } else {
            for (const rt::OverlayUnit& u : s.overlays.units()) {
                if (u.table.is_function_entry(pc)) {
                    units.push_back(u.name);
                }
            }
        }
        if (units.empty()) {
            throw UsageError("--stop-at-pc " + hex32(pc) + ": no unit has a function entry there");
        }
        for (const std::string& unit : units) {
            rt::OverrideSpec spec;
            spec.unit = unit;
            spec.address = pc;
            spec.kind = rt::OverrideKind::hook_before;
            spec.name = "stop-at-pc";
            spec.fn = [pc, unit](rt::OverrideCall& call) {
                rt::StopInfo info;
                info.reason = rt::StopReason::requested;
                info.pc = pc;
                info.address = pc;
                info.message = "--stop-at-pc: entered " + unit + " " + hex32(pc) + " from " +
                               hex32(rt::gpr_u32(*call.ctx, 31));
                call.machine->stop(std::move(info));
            };
            out.push_back(std::move(spec));
        }
    }
    return out;
}

std::optional<std::uint64_t> u64_field(const rt::MachineEvent& e, std::string_view key) {
    for (const auto& [k, v] : e.fields) {
        if (k != key) {
            continue;
        }
        if (const auto* u = std::get_if<std::uint64_t>(&v)) {
            return *u;
        }
        if (const auto* i = std::get_if<std::int64_t>(&v)) {
            return static_cast<std::uint64_t>(*i);
        }
    }
    return std::nullopt;
}

// The --stop-at-rpc entry an rpc_bind {sid, server} or rpc_call {sid, fno} event matches (decision 5 in
// Docs/milestones/M3-boots.md), or null.
const RpcStop* matching_rpc_stop(const Options& o, const rt::MachineEvent& e) {
    const bool bind = e.type == "rpc_bind";
    if (!bind && e.type != "rpc_call") {
        return nullptr;
    }
    const auto sid = u64_field(e, "sid");
    const auto fno = u64_field(e, "fno");
    for (const RpcStop& stop : o.stop_at_rpc) {
        if (!sid || *sid != stop.sid) {
            continue;
        }
        if (bind ? !stop.fno : (stop.fno && fno && *fno == *stop.fno)) {
            return &stop;
        }
    }
    return nullptr;
}

// How the machine loop ended.
struct Outcome {
    std::optional<rt::StopInfo> stop;
    bool requested = false; // a --stop-at-* stop (StopReason::requested)
    bool vblanks_reached = false;
    bool quit = false; // the window closed
    rt::SliceResult last;
    std::string gs_hash;
    std::exception_ptr error;
};

// The status channel's exit reason for how the run ended (the `end` event's "ended").
std::string_view status_reason(std::string_view ended) noexcept {
    return ended == "requested" ? std::string_view("stop") : ended;
}

int run_machine(const Options& command_line, const char* argv0, StatusChannel& status) {
    // M5 session A: the settings store (settings_wiring.h), read before the disc so that a damaged file
    // stops the run at once; its values become Options once the profile is known.
    std::optional<core::settings::Store> store = load_settings(command_line, executable_dir(argv0));
    Session s;
    setup_session(command_line, argv0, s);
    const core::RegionProfile* profile = s.profile();
    status.started(profile != nullptr ? std::string_view(profile->id) : std::string_view("dev"),
                   command_line.display == Display::window);
    Options effective = command_line;
    std::vector<std::string> applied_settings;
    if (store) {
        if (profile != nullptr) {
            store->use_profile(profile->id);
        }
        applied_settings = apply_settings(effective, *store);
        for (const std::string& w : store->warnings()) {
            std::fprintf(stderr, "settings: %s\n", w.c_str());
            status.warning(core::to_string(core::ErrorCode::settings_invalid), w);
        }
    }
    const Options& o = effective;

    // Everything that can fail on bad options or files, before any thread starts.
    std::optional<game::UntilTracker> until;
    if (o.until) {
        try {
            until.emplace(game::parse_until(*o.until, profile));
        } catch (const std::invalid_argument& e) {
            throw UsageError(e.what());
        }
    }
    std::unique_ptr<rt::ScriptInput> script;
    if (o.input_script) {
        try {
            script = std::make_unique<rt::ScriptInput>(rt::load_input_script(*o.input_script));
        } catch (const std::invalid_argument& e) {
            throw UsageError(std::string("--input-script: ") + e.what());
        }
    }
    // --movies play and nodecode run the sceMpeg HLE (movie_wiring.cpp), auto too when dq8-movie loads and
    // the sound modules can drain the movies' audio, else it skips.
    const auto movie_policy = game::resolve_movie_policy(o.movies, o.movie_decoder, profile);
    EventLog log(o.events, o.trace_events);

    // Disc.
    std::optional<core::iso::Image> image;
    std::unique_ptr<rt::IDiscSource> disc;
    if (o.iso) {
        image.emplace(core::iso::Image::open(*o.iso));
    } else if (o.disc) {
        image.emplace(core::iso::Image::open(*o.disc));
    }
    if (image) {
        disc = rt::make_iso_disc_source(*image);
    } else {
        disc = std::make_unique<NoDisc>();
    }

    // Input: a script, the window's live input, or nothing; optionally recorded.
    rt::LiveInput live;
    rt::IdleInput idle;
    rt::IInputSource* input = script                         ? static_cast<rt::IInputSource*>(script.get())
                              : o.display == Display::window ? static_cast<rt::IInputSource*>(&live)
                                                             : static_cast<rt::IInputSource*>(&idle);
    std::unique_ptr<rt::RecordingInput> recorder;
    if (o.record_input) {
        recorder = std::make_unique<rt::RecordingInput>(*input, *o.record_input);
        input = recorder.get();
    }

    // The window first (main thread), then the GS backend: either failing is exit 3.
#ifdef DQ8_HAVE_WINDOW
    std::unique_ptr<rt::IPlatform> platform;
    if (o.display == Display::window) {
        try {
            platform = rt::create_sdl_platform(rt::WindowConfig{});
        } catch (const std::exception& e) {
            throw BackendError(std::string("the window failed to open: ") + e.what());
        }
    }
#else
    if (o.display == Display::window) {
        throw BackendError("--window: this build has no SDL3 window (DQ8_BUILD_WINDOW=OFF)");
    }
#endif
    gs::GsPipelineConfig pipeline_config;
    pipeline_config.backend = make_backend(o, argv0);
    pipeline_config.inline_mode = o.gs == GsKind::null;
    gs::GsPipeline pipeline(std::move(pipeline_config));
    for (const AtVblank& d : o.dump_frames) {
        pipeline.request_frame_dump(d.vblank, d.path.string());
    }

    // The devices exist before the machine so that they outlive it (machine.h: an attached device must).
    std::vector<std::unique_ptr<rt::IDevice>> devices = rt::make_ee_devices(rt::EeDevicesConfig{&pipeline});
    rt::IopConfig iop_config;
    iop_config.disc = disc.get();
    iop_config.input = input;
    if (profile != nullptr) {
        iop_config.modules = game::iop_module_specs(*profile);
        iop_config.reset_image = game::iop_reset_image(*profile);
    }
    // The M4 sessions' additions (m4_wiring.h), before the IOP is built from its configuration.
    M4Wiring m4{o, profile, pipeline, iop_config, nullptr, {}, {}, {}, {}, {}, {}, {}};
#ifdef DQ8_HAVE_WINDOW
    m4.platform = platform.get();
#endif
    wire_audio(m4);
    wire_movies(m4);
    wire_saves(m4);
    wire_feedback(m4);
    rt::Iop iop(std::move(iop_config));
    std::unique_ptr<game::OverlayResidency> residency;
    std::unique_ptr<game::ModeWatcher> mode_watcher;
    std::unique_ptr<game::TitleWatcher> title_watcher;
    std::unique_ptr<game::MovieTimelineSkip> timeline_skip;
    if (profile != nullptr) {
        residency = std::make_unique<game::OverlayResidency>(
            game::OverlayResidencyConfig{profile, &s.overlays, o.verify_overlays});
        if (profile->overlay_window.mode_variable != 0) {
            mode_watcher = std::make_unique<game::ModeWatcher>(profile->overlay_window.mode_variable);
            if (profile->title_screen.variable != 0) {
                title_watcher = std::make_unique<game::TitleWatcher>(*profile);
            }
        }
        if (movie_policy.value_or(game::MoviePolicy::skip) == game::MoviePolicy::skip &&
            !profile->movie.timelines.empty()) {
            timeline_skip = std::make_unique<game::MovieTimelineSkip>(*profile);
        }
    }

    // The machine, then the devices in the fixed attach order: EE devices, IOP, game layer.
    rt::MachineConfig config;
    config.video = video_for(s);
    config.strict_kernel = o.strict_kernel;
    config.stop_at_first_syscall = o.stop_at_first_syscall;
    config.trace_dispatch = o.trace_dispatch;
    config.trace_mask = o.trace;
    config.vu0_shared = o.vu0_registers == "shared";
    s.memory.set_trap_mmio(o.trap_mmio);
    if (!o.json) {
        s.memory.set_mmio_sink([](const rt::MmioAccess& a) {
            std::fprintf(stderr, "mmio: first %s at %s (%s), pc %s\n", mmio_access_name(a).c_str(),
                         hex32(a.address).c_str(), std::string(rt::to_string(a.region)).c_str(),
                         hex32(a.pc).c_str());
        });
    }
    rt::Machine machine(s.memory, *s.dispatcher, config);
    s.dispatcher->set_machine(&machine);
    // The dispatcher outlives the machine: forget it before the machine goes.
    struct ForgetMachine {
        rt::Dispatcher& dispatcher;
        ~ForgetMachine() { dispatcher.set_machine(nullptr); }
    } forget_machine{*s.dispatcher};
    for (const auto& d : devices) {
        machine.attach(*d);
    }
    machine.attach(iop);
    if (residency) {
        machine.attach(*residency);
        s.dispatcher->install_override(residency->hook_override(), &s.memory);
    }
    if (title_watcher) {
        machine.attach(*title_watcher);
    }
    if (mode_watcher) {
        machine.attach(*mode_watcher);
    }
    if (timeline_skip) {
        machine.attach(*timeline_skip);
    }
    if (profile != nullptr) {
        for (rt::OverrideSpec& spec :
             game::movie_overrides(*profile, movie_policy.value_or(game::MoviePolicy::skip))) {
            s.dispatcher->install_override(std::move(spec), &s.memory);
        }
    }
    for (const auto& d : m4.devices) {
        machine.attach(*d);
    }
    for (rt::OverrideSpec& spec : m4.overrides) {
        s.dispatcher->install_override(std::move(spec), &s.memory);
    }
    for (rt::OverrideSpec& spec : stop_at_pc_overrides(o, s)) {
        s.dispatcher->install_override(std::move(spec), &s.memory);
    }

    // The distinct triggers the script waits for, and its marks as `mark` events.
    std::vector<rt::InputTrigger> script_triggers;
    if (script) {
        for (const rt::InputScriptLine& line : script->script().lines) {
            if (line.time == rt::InputScriptLine::Time::after &&
                std::find(script_triggers.begin(), script_triggers.end(), line.trigger) ==
                    script_triggers.end()) {
                script_triggers.push_back(line.trigger);
            }
        }
        // A mark also dumps the next frame into --dump-dir as <script>-<name>.png.
        const std::string stem = o.input_script ? o.input_script->stem().string() : std::string("script");
        script->set_mark_sink([&machine, &pipeline, &o, stem](const std::string& name, std::uint64_t vblank) {
            machine.emit(rt::MachineEvent{"mark", {}}.add("name", name));
            if (o.dump_dir) {
                pipeline.request_frame_dump(vblank + 1,
                                            (*o.dump_dir / (stem + "-" + name + ".png")).string());
            }
        });
    }
    machine.set_event_sink([&](const rt::MachineEvent& e) {
        const std::uint64_t vblank = machine.vblank_count();
        log.write(e, vblank, machine.clock().now());
        if (script) {
            for (const rt::InputTrigger& t : script_triggers) {
                if (game::event_matches_trigger(e, t)) {
                    script->note_trigger(t, vblank);
                }
            }
        }
        if (until) {
            until->observe(e, vblank);
        }
        if (const RpcStop* stop = o.stop_at_rpc.empty() ? nullptr : matching_rpc_stop(o, e)) {
            rt::StopInfo info;
            info.reason = rt::StopReason::requested;
            info.address = stop->sid;
            info.message = "--stop-at-rpc: " + e.type + " of server " + hex32(stop->sid) +
                           (stop->fno ? ", function " + std::to_string(*stop->fno) : std::string());
            machine.stop(std::move(info));
        }
    });

    if (!o.json) {
        print_setup(s);
        std::fflush(stdout); // before the run's stderr diagnostics
    }
    {
        rt::MachineEvent start{"start", {}};
        start.add("mode", s.mode)
            .add("entry", hex32(s.elf.entry))
            .add("cost_model", std::string(config.cost.name));
        if (profile != nullptr) {
            start.add("profile", profile->id);
        }
        start.add("input", std::string(script ? "script" : o.display == Display::window ? "live" : "idle"));
        for (const auto& add_fields : m4.start_fields) {
            add_fields(start);
        }
        log.write(start, 0, 0);
    }

    // The machine thread.
    std::atomic<bool> quit{false};
    std::atomic<bool> done{false};
    Outcome outcome;
    std::vector<AtVblank> ram_dumps = o.dump_ram_at;
    const auto machine_loop = [&] {
        try {
            machine.boot(s.elf.entry);
            Limiter limiter(o.speed);
            std::uint64_t vblank = 0;
            for (;;) {
                rt::RunLimits limits;
                limits.until_vblank = o.vblanks ? std::min(vblank + 1, *o.vblanks) : vblank + 1;
                limits.max_dispatches = o.max_dispatches;
                const rt::SliceResult r = machine.run(limits);
                outcome.last = r;
                status.guest_time(seconds_of(r.cycles));
                if (r.vblank != vblank) {
                    vblank = r.vblank;
                    if (o.display != Display::window) {
                        status.frame(); // headless, a vblank stands for a frame
                    }
                    outcome.gs_hash = pipeline.stream_hash();
                    log.write(rt::MachineEvent{"gs", {}}
                                  .add("hash", outcome.gs_hash)
                                  .add("vsyncs", pipeline.vsyncs()),
                              vblank, r.cycles);
                    for (const AtVblank& d : ram_dumps) {
                        if (d.vblank == vblank) {
                            write_ram(s.memory, d.path);
                            log.write(rt::MachineEvent{"ram_dump", {}}.add("sha1", ram_sha1(s.memory)),
                                      vblank, r.cycles);
                        }
                    }
                    for (const AtVblank& d : o.dump_iop_ram_at) {
                        if (d.vblank == vblank) {
                            const std::span<const std::uint8_t> iop_ram = iop.ram();
                            std::ofstream out(d.path, std::ios::binary | std::ios::trunc);
                            out.write(reinterpret_cast<const char*>(iop_ram.data()),
                                      static_cast<std::streamsize>(iop_ram.size()));
                            if (!out) {
                                throw core::Error(core::ErrorCode::io_write,
                                                  "cannot write the IOP RAM dump " + d.path.string());
                            }
                            log.write(rt::MachineEvent{"iop_ram_dump", {}}.add(
                                          "sha1", core::to_hex(core::Sha1::of(iop_ram))),
                                      vblank, r.cycles);
                        }
                    }
                }
                if (until) {
                    until->at_vblank(vblank);
                }
                if (r.stopped) {
                    outcome.stop = r.stop;
                    outcome.requested = r.stop && r.stop->reason == rt::StopReason::requested;
                    break;
                }
                if (until && until->met()) {
                    break;
                }
                if (o.vblanks && vblank >= *o.vblanks) {
                    outcome.vblanks_reached = true;
                    break;
                }
                if (quit.load()) {
                    outcome.quit = true;
                    break;
                }
                limiter.wait(seconds_of(r.cycles));
            }
            outcome.gs_hash = pipeline.stream_hash();
        } catch (...) {
            outcome.error = std::current_exception();
        }
        done.store(true);
    };

    for (const auto& start_hook : m4.on_start) {
        start_hook();
    }
    std::thread machine_thread(machine_loop);
#ifdef DQ8_HAVE_WINDOW
    if (platform) {
        std::uint64_t presented = ~std::uint64_t{0};
        while (!done.load()) {
            if (!platform->pump_events()) {
                quit.store(true);
            }
            live.publish(0, platform->pad(0));
            live.publish(1, platform->pad(1));
            for (const auto& frame_hook : m4.on_frame) {
                frame_hook();
            }
            gs::FrameImage frame;
            if (pipeline.latest_frame(frame) && frame.field != presented) {
                platform->present(frame, 4.0 / 3.0);
                presented = frame.field;
                status.frame();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }
#endif
    machine_thread.join();
    for (const auto& stop_hook : m4.on_stop) {
        stop_hook();
    }
    if (recorder) {
        recorder->flush();
    }
    if (outcome.error) {
        std::rethrow_exception(outcome.error);
    }

    // Exit status and the report.
    int code = exit_ok;
    std::string ended;
    if (outcome.stop) {
        ended = outcome.requested ? "requested" : "stop";
        code = outcome.requested || (o.stop_at_first_syscall && stopped_at_syscall(*outcome.stop))
                   ? exit_ok
                   : exit_guest_stop;
    } else if (until && until->met()) {
        ended = "until";
    } else if (outcome.quit) {
        ended = "window-closed";
    } else {
        ended = "vblanks";
        code = until ? exit_until_not_met : exit_ok;
    }
    const std::string ram = ram_sha1(s.memory);
    {
        rt::MachineEvent end{"end", {}};
        end.add("ended", ended)
            .add("gs_hash", outcome.gs_hash)
            .add("ram_sha1", ram)
            .add("exit_code", std::int64_t{code});
        if (outcome.stop) {
            end.add("reason", std::string(rt::to_string(outcome.stop->reason)));
        }
        for (const auto& add_fields : m4.end_fields) {
            add_fields(end);
        }
        log.write(end, outcome.last.vblank, outcome.last.cycles);
        log.flush();
    }

    const R5900Context& ctx = machine.ee().context();
    if (o.json) {
        JsonWriter j;
        j.begin_object();
        j.member("ok", code == exit_ok);
        json_setup(j, s);
        j.key("machine");
        j.begin_object();
        j.member("gs", gs_name(o.gs));
        j.member("cost_model", config.cost.name);
        j.member("vblanks", outcome.last.vblank);
        j.member("cycles", outcome.last.cycles);
        j.member("gs_hash", outcome.gs_hash);
        j.member("gs_vsyncs", pipeline.vsyncs());
        j.member("ram_sha1", ram);
        for (const auto& device : devices) {
            if (const auto* vu = dynamic_cast<const rt::VuUnits*>(device.get())) {
                // Micro programs run (VIF MSCAL/MSCNT and, for VU0, VCALLMS), their cycles, VU1's XGKICKs.
                j.key("vu");
                j.begin_object();
                for (unsigned unit = 0; unit < 2; ++unit) {
                    const rt::VuUnits::UnitStats& st = vu->stats(unit);
                    j.key(unit == 0 ? "vu0" : "vu1");
                    j.begin_object();
                    j.member("programs", st.programs);
                    j.member("cycles", st.cycles);
                    j.member("xgkicks", st.xgkicks);
                    j.end_object();
                }
                j.end_object();
            }
        }
        j.end_object();
        j.member("ended", ended);
        if (until) {
            j.key("until");
            j.begin_object();
            j.member("condition", until->condition().text);
            j.member("met", until->met());
            if (until->met_at()) {
                j.member("at", *until->met_at());
            }
            j.end_object();
        }
        j.key("overrides");
        j.begin_array();
        for (const rt::OverrideReport& r : s.dispatcher->overrides()) {
            j.begin_object();
            j.member("name", r.name);
            j.member("unit", r.unit);
            j.member("address", hex32(r.address));
            j.member("kind", rt::to_string(r.kind));
            j.member("calls", r.calls);
            j.end_object();
        }
        j.end_array();
        if (script) {
            j.key("input_script");
            j.begin_object();
            j.member("lines_applied", script->applied());
            j.member("finished", script->finished());
            if (const auto w = script->waiting_line()) {
                j.member("waiting_line", *w);
            }
            j.end_object();
        }
        if (outcome.stop) {
            json_stop(j, *outcome.stop);
        }
        j.member("dispatches", outcome.last.dispatches);
        json_context(j, ctx);
        json_mmio(j, s.memory);
        if (store) {
            j.key("settings");
            j.begin_array();
            for (const std::string& applied : applied_settings) {
                j.value(applied);
            }
            j.end_array();
        }
        j.member("exit_code", code);
        j.end_object();
        std::printf("%s\n", j.str().c_str());
    } else if (outcome.stop) {
        print_stop(*outcome.stop, outcome.last.dispatches, ctx, s.memory);
        std::printf("vblanks %llu, gs %s, ram %s\n", static_cast<unsigned long long>(outcome.last.vblank),
                    outcome.gs_hash.c_str(), ram.c_str());
    } else {
        std::printf("ran %llu vblanks (%s), %llu dispatches\ngs %s\nram %s\n",
                    static_cast<unsigned long long>(outcome.last.vblank), ended.c_str(),
                    static_cast<unsigned long long>(outcome.last.dispatches), outcome.gs_hash.c_str(),
                    ram.c_str());
        if (until && !until->met()) {
            std::printf("--until %s was not met\n", until->condition().text.c_str());
        }
    }
    std::fflush(stdout);
    status.exit(code, status_reason(ended));
    return code;
}

} // namespace

int run_game(const Options& o, const char* argv0) {
    StatusChannel status(o.status);
    try {
        return run_machine(o, argv0, status);
    } catch (...) {
        status.exit_on(std::current_exception());
        throw;
    }
}

} // namespace dq8::host
