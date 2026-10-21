// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// dq8-game's command line (src/host/options.*), its JSON-lines event format and the limiter.
#include "host/event_log.h"
#include "host/limiter.h"
#include "host/options.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace dq8::host;

Options parse(std::initializer_list<const char*> args) {
    std::vector<const char*> argv{"dq8-game"};
    argv.insert(argv.end(), args.begin(), args.end());
    return parse_options(static_cast<int>(argv.size()), argv.data());
}

std::string error_of(std::initializer_list<const char*> args) {
    try {
        parse(args);
    } catch (const UsageError& e) {
        return e.what();
    }
    return "<accepted>";
}

} // namespace

TEST_CASE("host options: M2 command lines parse as before", "[host]") {
    const Options o = parse({"--elf", "a.elf", "--module-dir", "mods", "--stop-at-first-syscall", "--json",
                             "--max-dispatches", "0x10", "--trace-dispatch", "--trap-mmio"});
    CHECK(o.elf == fs::path("a.elf"));
    CHECK(o.module_dir == fs::path("mods"));
    CHECK(o.stop_at_first_syscall);
    CHECK(o.json);
    CHECK(o.max_dispatches == 16);
    CHECK(o.trace_dispatch);
    CHECK(o.trap_mmio);
    CHECK_FALSE(o.uses_machine);
    CHECK(o.display == Display::headless);
    CHECK(o.gs == GsKind::null);
    CHECK_FALSE(o.speed); // headless: as fast as possible

    // M2's --window BASE:END is the developer overlay window; --resident needs it.
    const Options w = parse(
        {"--elf", "a.elf", "--module-dir", "m", "--window", "0x00300000:0x00310000", "--resident", "1"});
    REQUIRE(w.overlay_window);
    CHECK(w.overlay_window->base == 0x00300000u);
    CHECK(w.overlay_window->end == 0x00310000u);
    CHECK(w.resident == 1u);
    CHECK(w.display == Display::headless);
    CHECK_FALSE(w.uses_machine);
    CHECK(parse({"--elf", "a", "--module-dir", "m", "--window=0x100:0x200"}).overlay_window->end == 0x200u);
    CHECK(parse({"--elf", "a", "--module-dir", "m", "--overlay-window", "0x100:0x200"}).overlay_window);
}

TEST_CASE("host options: --window alone is the SDL window", "[host]") {
    const Options o = parse({"--iso", "g.iso", "--module-dir", "m", "--window"});
    CHECK(o.display == Display::window);
    CHECK(o.gs == GsKind::parallel);
    CHECK(o.speed == 1.0);
    CHECK(o.uses_machine);
    CHECK(parse({"--iso", "g.iso", "--window", "--module-dir", "m"}).display == Display::window);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--window", "--headless"}).find("at most one") !=
          std::string::npos);
}

TEST_CASE("host options: M3 run options", "[host]") {
    const Options o = parse({"--iso",
                             "g.iso",
                             "--module-dir",
                             "m",
                             "--headless",
                             "--gs",
                             "trace",
                             "--vblanks",
                             "6000",
                             "--until",
                             "overlay:TITLE",
                             "--input-script",
                             "in.dq8in",
                             "--record-input",
                             "out.dq8in",
                             "--speed",
                             "1.5",
                             "--movies",
                             "stop",
                             "--events",
                             "-",
                             "--trace",
                             "kernel,rpc,gs",
                             "--strict-kernel",
                             "--verify-overlays",
                             "--stop-at-pc",
                             "0x00100320",
                             "--stop-at-pc",
                             "0x00100330",
                             "--stop-at-rpc",
                             "0x80000001",
                             "--dump-frame",
                             "600:a.png",
                             "--dump-frame",
                             "601:b.png",
                             "--dump-ram-at",
                             "100:ram.bin"});
    CHECK(o.uses_machine);
    CHECK(o.gs == GsKind::trace);
    CHECK(o.vblanks == 6000u);
    CHECK(o.until == "overlay:TITLE");
    CHECK(o.input_script == fs::path("in.dq8in"));
    CHECK(o.record_input == fs::path("out.dq8in"));
    CHECK(o.speed == 1.5);
    CHECK(o.movies == "stop");
    CHECK(o.events == "-");
    using dq8::rt::trace_bit;
    using dq8::rt::TraceCategory;
    CHECK(o.trace ==
          (trace_bit(TraceCategory::kernel) | trace_bit(TraceCategory::rpc) | trace_bit(TraceCategory::gs)));
    CHECK(o.strict_kernel);
    CHECK(o.verify_overlays);
    CHECK(o.stop_at_pc == std::vector<std::uint32_t>{0x00100320u, 0x00100330u});
    CHECK(o.stop_at_rpc == std::vector<RpcStop>{{0x80000001u, std::nullopt}});
    CHECK_FALSE(o.trace_events);

    // SID:FNO stops at a call, and rpc_call events need RPC tracing on.
    const Options call =
        parse({"--iso", "g", "--module-dir", "m", "--stop-at-rpc", "0x80000001:2", "--trace-events"});
    CHECK(call.stop_at_rpc == std::vector<RpcStop>{{0x80000001u, 2u}});
    CHECK(call.trace == trace_bit(TraceCategory::rpc));
    CHECK(call.trace_events);
    REQUIRE(o.dump_frames.size() == 2);
    CHECK(o.dump_frames[1].vblank == 601);
    CHECK(o.dump_frames[1].path == fs::path("b.png"));
    REQUIRE(o.dump_ram_at.size() == 1);
    CHECK(o.dump_ram_at[0].vblank == 100);

    CHECK_FALSE(parse({"--iso", "g", "--module-dir", "m", "--speed", "max", "--window"}).speed);
    CHECK(parse({"--iso", "g", "--module-dir", "m", "--gs-trace", "t.gs"}).gs == GsKind::trace);
    CHECK(parse({"--iso", "g", "--module-dir", "m", "--gs", "parallel", "--ssaa", "4"}).ssaa == 4);
    CHECK(parse({"--elf", "e", "--module-dir", "m", "--disc", "d.iso"}).disc == fs::path("d.iso"));
    CHECK(parse({"--iso", "g", "--module-dir", "m"}).movies == "skip");
    CHECK(parse({"--iso", "g", "--module-dir", "m", "--movies", "blank"}).movies == "blank");
}

TEST_CASE("host options: problems are usage errors", "[host]") {
    CHECK(error_of({"--module-dir", "m"}).find("exactly one of --iso and --elf") != std::string::npos);
    CHECK(error_of({"--iso", "g", "--elf", "e", "--module-dir", "m"}).find("exactly one") !=
          std::string::npos);
    CHECK(error_of({"--iso", "g"}).find("--module-dir is required") != std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--vblanks", "1", "--vblanks", "2"})
              .find("given twice") != std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--json", "--json"}).find("given twice") !=
          std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--frobnicate"}).find("unknown option") !=
          std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "stray"}).find("unexpected argument") !=
          std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--vblanks"}).find("needs a value") !=
          std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--vblanks", "0"}).find("at least 1") !=
          std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--vblanks", "ten"}).find("not a number") !=
          std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--gs", "vulkan"}).find("null, trace or parallel") !=
          std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--gs", "null", "--gs-trace", "t"})
              .find("needs --gs trace") != std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--ssaa", "3", "--gs", "parallel"})
              .find("1, 2, 4, 8 or 16") != std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--ssaa", "2"}).find("--gs parallel") !=
          std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--speed", "0"}).find("--speed") != std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--speed", "fast"}).find("--speed") !=
          std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--movies", "play"}).find("skip, blank or stop") !=
          std::string::npos);
    CHECK(
        error_of({"--iso", "g", "--module-dir", "m", "--trace", "kernel,vu"}).find("unknown category 'vu'") !=
        std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--dump-frame", "600"}).find("N:PATH") !=
          std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--stop-at-pc", "0x102"}).find("4-aligned") !=
          std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--disc", "d"}).find("developer-mode") !=
          std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--window", "0x100:0x200"}).find("developer-mode") !=
          std::string::npos);
    CHECK(error_of({"--elf", "e", "--module-dir", "m", "--regions", "r"}).find("--iso only") !=
          std::string::npos);
    CHECK(error_of({"--elf", "e", "--module-dir", "m", "--resident", "1"}).find("needs --overlay-window") !=
          std::string::npos);
    CHECK(
        error_of({"--elf", "e", "--module-dir", "m", "--overlay-window", "0x200:0x100"}).find("below END") !=
        std::string::npos);
    CHECK(error_of({"--iso", "g", "--module-dir", "m", "--input-script", "x", "--record-input", "x"})
              .find("overwrite") != std::string::npos);
    CHECK(parse({"--help"}).help);
    CHECK(error_of({"--version", "--bogus"}).find("unknown option") != std::string::npos);
}

TEST_CASE("host events: one JSON object per event, with vblank and cycles first", "[host]") {
    const dq8::rt::MachineEvent e = dq8::rt::MachineEvent{"overlay_resident", {}}
                                        .add("name", std::string("TITLE"))
                                        .add("overlay_id", std::uint64_t{6})
                                        .add("delta", std::int64_t{-2})
                                        .add("verified", false);
    CHECK(EventLog::format(e, 120, 590413824) ==
          R"({"ev":"overlay_resident","vblank":120,"cycles":590413824,"name":"TITLE","overlay_id":6,)"
          R"("delta":-2,"verified":false})");
    const dq8::rt::MachineEvent trace = dq8::rt::MachineEvent{"trace", {}}
                                            .add("cat", std::string("rpc"))
                                            .add("msg", std::string("bind 0x80000001"));
    CHECK(EventLog::format_trace(trace, 42) == "trace 42 [rpc] bind 0x80000001");
    EventLog none(std::nullopt, false);
    CHECK_FALSE(none.enabled());
}

TEST_CASE("host limiter: deadlines follow virtual time at the chosen speed", "[host]") {
    using namespace std::chrono;
    const Limiter::Clock::time_point t0{seconds(100)};
    Limiter unlimited(std::nullopt);
    CHECK(unlimited.unlimited());
    CHECK_FALSE(unlimited.deadline(1.0, t0));

    Limiter half(0.5);
    CHECK_FALSE(half.deadline(10.0, t0)); // the first call sets the origin
    const auto due = half.deadline(10.5, t0);
    REQUIRE(due);
    CHECK(duration_cast<milliseconds>(*due - t0).count() == 1000); // 0.5 s of virtual time at half speed
    CHECK_FALSE(half.deadline(10.5, t0 + seconds(1)));             // already due
    CHECK_FALSE(half.deadline(11.0, t0 + seconds(5)));             // far behind: re-base, no catch-up burst
    CHECK(half.rebases() == 1);
    const auto after = half.deadline(11.125, t0 + seconds(5));
    REQUIRE(after);
    CHECK(duration_cast<milliseconds>(*after - (t0 + seconds(5))).count() == 250);
}
