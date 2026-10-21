// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "host/options.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string_view>

namespace dq8::host {

namespace {

// Options that take a value, and those of them that may be given more than once.
const std::set<std::string, std::less<>> value_options{
    "iso",        "elf",         "regions",        "module-dir",   "overlay-window",
    "resident",   "disc",        "max-dispatches", "gs",           "vblanks",
    "until",      "dump-frame",  "gs-trace",       "input-script", "record-input",
    "speed",      "ssaa",        "movies",         "events",       "trace",
    "stop-at-pc", "stop-at-rpc", "dump-ram-at"};
const std::set<std::string, std::less<>> repeatable{"dump-frame", "stop-at-pc", "stop-at-rpc", "dump-ram-at"};
const std::set<std::string, std::less<>> flag_options{
    "stop-at-first-syscall", "trace-dispatch",  "trap-mmio",   "json", "help", "version", "headless",
    "strict-kernel",         "verify-overlays", "trace-events"};
// Options that only the M3 machine understands.
const std::set<std::string, std::less<>> machine_options{
    "disc",        "gs",           "vblanks",       "until",           "dump-frame",
    "gs-trace",    "input-script", "record-input",  "speed",           "ssaa",
    "movies",      "events",       "trace",         "stop-at-pc",      "stop-at-rpc",
    "dump-ram-at", "headless",     "strict-kernel", "verify-overlays", "trace-events"};

rt::OverlayWindowSpec parse_window(std::string_view v) {
    const std::size_t colon = v.find(':');
    if (colon == std::string_view::npos) {
        throw UsageError("--overlay-window needs BASE:END");
    }
    const std::uint32_t base = parse_u32(v.substr(0, colon), "--overlay-window BASE");
    const std::uint32_t end = parse_u32(v.substr(colon + 1), "--overlay-window END");
    if (base >= end || (base & 3u) != 0 || (end & 3u) != 0) {
        throw UsageError("--overlay-window " + std::string(v) +
                         ": BASE must be below END and both 4-aligned");
    }
    return {base, end};
}

AtVblank parse_at_vblank(std::string_view v, std::string_view what) {
    const std::size_t colon = v.find(':');
    if (colon == std::string_view::npos || colon + 1 >= v.size()) {
        throw UsageError(std::string(what) + " needs N:PATH, e.g. 600:frame.png");
    }
    return {parse_number(v.substr(0, colon), what), fs::path(std::string(v.substr(colon + 1)))};
}

std::uint32_t parse_trace(std::string_view v) {
    std::uint32_t mask = 0;
    std::size_t start = 0;
    while (start <= v.size()) {
        const std::size_t comma = v.find(',', start);
        const std::string_view name =
            v.substr(start, comma == std::string_view::npos ? v.npos : comma - start);
        bool known = false;
        for (const rt::TraceCategory c :
             {rt::TraceCategory::kernel, rt::TraceCategory::sif, rt::TraceCategory::rpc,
              rt::TraceCategory::dma, rt::TraceCategory::gs}) {
            if (name == rt::to_string(c)) {
                mask |= rt::trace_bit(c);
                known = true;
            }
        }
        if (!known) {
            throw UsageError("--trace: unknown category '" + std::string(name) +
                             "' (kernel, sif, rpc, dma, gs)");
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return mask;
}

std::optional<double> parse_speed(std::string_view v) {
    if (v == "max") {
        return std::nullopt;
    }
    double speed = 0;
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), speed);
    if (v.empty() || ec != std::errc() || end != v.data() + v.size() || !std::isfinite(speed) ||
        speed < 0.05 || speed > 100.0) {
        throw UsageError("--speed '" + std::string(v) +
                         "': a factor from 0.05 to 100 (1.0 = real time), or max");
    }
    return speed;
}

} // namespace

std::uint64_t parse_number(std::string_view text, std::string_view what) {
    int base = 10;
    std::string_view digits = text;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits.remove_prefix(2);
    }
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
    if (digits.empty() || ec != std::errc() || end != digits.data() + digits.size()) {
        throw UsageError(std::string(what) + ": '" + std::string(text) + "' is not a number");
    }
    return value;
}

std::uint32_t parse_u32(std::string_view text, std::string_view what) {
    const std::uint64_t v = parse_number(text, what);
    if (v > 0xFFFFFFFFu) {
        throw UsageError(std::string(what) + ": '" + std::string(text) + "' does not fit in 32 bits");
    }
    return static_cast<std::uint32_t>(v);
}

int usage(std::FILE* out) {
    std::fputs(
        "usage: dq8-game --iso <image> --module-dir <dir> [--regions DIR] [options]\n"
        "       dq8-game --elf <file> --module-dir <dir> [--overlay-window BASE:END [--resident ID]]\n"
        "                [--disc <image>] [options]\n"
        "\n"
        "  --iso <image>            verify the disc against the region profiles, load the seven unit "
        "modules\n"
        "                           named after the profile and boot the game\n"
        "  --elf <file>             developer mode: any MIPS ELF and every dq8_unit_* module in "
        "--module-dir\n"
        "  --module-dir <dir>       where the game modules are (dq8ctl recomp build writes them)\n"
        "  --regions <dir>          region profiles (default: data/regions near the current directory or "
        "the\n"
        "                           executable)\n"
        "  --overlay-window B:E     developer mode: the overlay window (M2 spelling: --window B:E)\n"
        "  --resident ID            developer mode: the overlay id resident in the window at start\n"
        "  --disc <image>           developer mode: a disc image the IOP serves (not verified)\n"
        "\n"
        "run:\n"
        "  --headless | --window    no window (default) or an SDL3 window with live input\n"
        "  --gs null|trace|parallel GS backend (default: null headless, parallel with --window)\n"
        "  --gs-trace <file>        with --gs trace: write the GS packet stream (PCSX2 dump packet format)\n"
        "  --ssaa N                 with --gs parallel: super-sampling 1, 2, 4, 8 or 16\n"
        "  --vblanks N              end the run after N vblanks\n"
        "  --until COND             end the run when COND holds: overlay:NAME, mode:N, vblank:N, title\n"
        "  --speed F|max            1.0 is real time; max (default headless) runs as fast as it can;\n"
        "                           default with --window: 1.0\n"
        "  --input-script <file>    scripted pad input (.dq8in, Docs/formats/dq8-input-script.md)\n"
        "  --record-input <file>    write the input the run read as a .dq8in script\n"
        "  --movies skip|blank|stop movies end at once, their scene timelines too (default); blank: the\n"
        "                           player returns at once but the scene's clock runs on black (as in M3);\n"
        "                           stop: the first player call stops the guest\n"
        "  --dump-frame N:PATH      write the scanout of vblank N as PNG (repeatable)\n"
        "  --dump-ram-at N:PATH     write the 32 MiB of RAM after N vblanks (repeatable)\n"
        "  --events FILE|-          JSON-lines events (deterministic fields only)\n"
        "  --trace LIST             trace categories, printed on stderr: kernel,sif,rpc,dma,gs\n"
        "  --trace-events           also write the trace events into --events\n"
        "  --strict-kernel          noop kernel services stop instead\n"
        "  --verify-overlays        hash each loaded overlay against the profile and watch its text\n"
        "  --stop-at-pc ADDR        stop when the function at ADDR is entered (repeatable)\n"
        "  --stop-at-rpc SID[:FNO]  stop when the game binds IOP RPC server SID, or calls its function FNO\n"
        "                           (repeatable)\n"
        "  --stop-at-first-syscall  stop at the first kernel call, implemented or not (exit 0 there)\n"
        "  --max-dispatches N       stop after N dispatches (0: no limit)\n"
        "  --trace-dispatch         print every dispatch on stderr\n"
        "  --trap-mmio              stop at the first access to an unhandled device register\n"
        "  --json                   print one JSON object on stdout at the end\n"
        "  --version | --help\n"
        "\n"
        "exit status: 0 the run ended as asked (--until met, --vblanks reached, a --stop-at-* or\n"
        "--stop-at-first-syscall stop, the window closed), 1 any other guest stop, 2 usage, I/O, disc\n"
        "verification or module validation, 3 the GS backend or the window failed to start,\n"
        "4 --vblanks ran out before --until held\n",
        out);
    return out == stdout ? 0 : exit_error;
}

Options parse_options(int argc, const char* const* argv) {
    std::map<std::string, std::vector<std::string>, std::less<>> values;
    std::set<std::string, std::less<>> flags;
    bool display_window = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.size() <= 2 || arg.compare(0, 2, "--") != 0) {
            throw UsageError("unexpected argument '" + arg + "'");
        }
        std::string name = arg.substr(2);
        std::optional<std::string> value;
        if (const std::size_t eq = name.find('='); eq != std::string::npos) {
            value = name.substr(eq + 1);
            name.erase(eq);
        }
        if (name == "window") {
            // M3's --window is a flag; M2's --window BASE:END (developer mode) still works: a following
            // BASE:END argument, or =BASE:END, makes it the overlay window.
            const bool next_is_range = !value && i + 1 < argc &&
                                       std::string_view(argv[i + 1]).find(':') != std::string_view::npos &&
                                       std::string_view(argv[i + 1]).rfind("--", 0) != 0;
            if (!value && !next_is_range) {
                if (display_window) {
                    throw UsageError("--window given twice");
                }
                display_window = true;
                continue;
            }
            name = "overlay-window";
            if (!value) {
                value = argv[++i];
            }
        }
        if (value_options.count(name) != 0) {
            if (!value) {
                if (i + 1 >= argc) {
                    throw UsageError("--" + name + " needs a value");
                }
                value = argv[++i];
            }
            auto& list = values[name];
            if (!list.empty() && repeatable.count(name) == 0) {
                throw UsageError("--" + name + " given twice");
            }
            list.push_back(*value);
        } else if (flag_options.count(name) != 0 && !value) {
            if (!flags.insert(name).second) {
                throw UsageError("--" + name + " given twice");
            }
        } else {
            throw UsageError("unknown option '" + arg + "'");
        }
    }

    Options o;
    o.help = flags.count("help") != 0;
    o.version = flags.count("version") != 0;
    if (o.help || o.version) {
        return o;
    }
    o.json = flags.count("json") != 0;
    o.trap_mmio = flags.count("trap-mmio") != 0;
    o.stop_at_first_syscall = flags.count("stop-at-first-syscall") != 0;
    o.trace_dispatch = flags.count("trace-dispatch") != 0;
    o.strict_kernel = flags.count("strict-kernel") != 0;
    o.verify_overlays = flags.count("verify-overlays") != 0;
    o.trace_events = flags.count("trace-events") != 0;
    const auto get = [&](std::string_view key) -> const std::string* {
        const auto it = values.find(key);
        return it != values.end() ? &it->second.front() : nullptr;
    };
    const auto all = [&](std::string_view key) -> std::vector<std::string> {
        const auto it = values.find(key);
        return it != values.end() ? it->second : std::vector<std::string>{};
    };
    for (const auto& [name, list] : values) {
        o.uses_machine = o.uses_machine || machine_options.count(name) != 0;
    }
    for (const std::string& name : flags) {
        o.uses_machine = o.uses_machine || machine_options.count(name) != 0;
    }
    o.uses_machine = o.uses_machine || display_window;

    if (const auto* v = get("iso")) {
        o.iso = *v;
    }
    if (const auto* v = get("elf")) {
        o.elf = *v;
    }
    if (const auto* v = get("regions")) {
        o.regions = *v;
    }
    if (const auto* v = get("module-dir")) {
        o.module_dir = *v;
    }
    if (const auto* v = get("disc")) {
        o.disc = *v;
    }
    if (const auto* v = get("max-dispatches")) {
        o.max_dispatches = parse_number(*v, "--max-dispatches");
    }
    if (const auto* v = get("overlay-window")) {
        o.overlay_window = parse_window(*v);
    }
    if (const auto* v = get("resident")) {
        o.resident = parse_u32(*v, "--resident");
    }

    if (display_window && flags.count("headless") != 0) {
        throw UsageError("give at most one of --headless and --window");
    }
    o.display = display_window ? Display::window : Display::headless;
    o.gs = o.display == Display::window ? GsKind::parallel : GsKind::null;
    if (const auto* v = get("gs-trace")) {
        o.gs_trace = *v;
        o.gs = GsKind::trace;
    }
    if (const auto* v = get("gs")) {
        if (*v == "null") {
            o.gs = GsKind::null;
        } else if (*v == "trace") {
            o.gs = GsKind::trace;
        } else if (*v == "parallel") {
            o.gs = GsKind::parallel;
        } else {
            throw UsageError("--gs '" + *v + "': null, trace or parallel");
        }
        if (o.gs_trace && o.gs != GsKind::trace) {
            throw UsageError("--gs-trace writes the trace backend's stream; it needs --gs trace");
        }
    }
    if (const auto* v = get("ssaa")) {
        o.ssaa = parse_u32(*v, "--ssaa");
        if (o.ssaa != 1 && o.ssaa != 2 && o.ssaa != 4 && o.ssaa != 8 && o.ssaa != 16) {
            throw UsageError("--ssaa " + *v + ": 1, 2, 4, 8 or 16");
        }
        if (o.gs != GsKind::parallel) {
            throw UsageError("--ssaa applies to --gs parallel");
        }
    }
    if (const auto* v = get("vblanks")) {
        o.vblanks = parse_number(*v, "--vblanks");
        if (*o.vblanks == 0) {
            throw UsageError("--vblanks must be at least 1");
        }
    }
    if (const auto* v = get("until")) {
        o.until = *v;
    }
    if (const auto* v = get("speed")) {
        o.speed = parse_speed(*v);
    } else if (o.display == Display::window) {
        o.speed = 1.0;
    }
    if (const auto* v = get("movies")) {
        if (*v != "skip" && *v != "blank" && *v != "stop") {
            throw UsageError("--movies '" + *v + "': skip, blank or stop (playback comes in M4)");
        }
        o.movies = *v;
    }
    if (const auto* v = get("events")) {
        o.events = *v;
    }
    if (const auto* v = get("trace")) {
        o.trace = parse_trace(*v);
    }
    if (const auto* v = get("input-script")) {
        o.input_script = *v;
    }
    if (const auto* v = get("record-input")) {
        o.record_input = *v;
    }
    for (const std::string& v : all("dump-frame")) {
        o.dump_frames.push_back(parse_at_vblank(v, "--dump-frame"));
    }
    for (const std::string& v : all("dump-ram-at")) {
        o.dump_ram_at.push_back(parse_at_vblank(v, "--dump-ram-at"));
    }
    for (const std::string& v : all("stop-at-pc")) {
        const std::uint32_t pc = parse_u32(v, "--stop-at-pc");
        if ((pc & 3u) != 0) {
            throw UsageError("--stop-at-pc " + v + ": not 4-aligned");
        }
        o.stop_at_pc.push_back(pc);
    }
    for (const std::string& v : all("stop-at-rpc")) {
        RpcStop stop;
        const std::size_t colon = v.find(':');
        stop.sid = parse_u32(std::string_view(v).substr(0, colon), "--stop-at-rpc SID");
        if (colon != std::string::npos) {
            stop.fno = parse_u32(std::string_view(v).substr(colon + 1), "--stop-at-rpc FNO");
            // rpc_call events exist only while RPC tracing is on (Docs/milestones/M3-boots.md, decision 5).
            o.trace |= rt::trace_bit(rt::TraceCategory::rpc);
        }
        o.stop_at_rpc.push_back(stop);
    }

    if (o.iso.has_value() == o.elf.has_value()) {
        throw UsageError("give exactly one of --iso and --elf");
    }
    if (!o.module_dir) {
        throw UsageError("--module-dir is required");
    }
    if (o.iso && (o.overlay_window || o.resident || o.disc)) {
        throw UsageError("--overlay-window, --resident and --disc are developer-mode options (--elf); with "
                         "--iso the region profile and the disc give them");
    }
    if (o.elf && o.regions) {
        throw UsageError("--regions applies to --iso only");
    }
    if (o.resident && !o.overlay_window) {
        throw UsageError("--resident needs --overlay-window");
    }
    if (o.input_script && o.record_input && *o.input_script == *o.record_input) {
        throw UsageError("--record-input would overwrite the --input-script file");
    }
    return o;
}

} // namespace dq8::host
