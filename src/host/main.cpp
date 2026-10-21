// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// dq8-game: the game executable. In M2 it verifies the disc, loads and validates the locally built game
// modules, places the boot ELF in guest memory and runs the main unit from its entry until the guest stops
// (the first unimplemented syscall). Developer mode (--elf) does the same for any ELF and a directory of unit
// modules, without a disc or a region profile.
//
// Exit status: 0 stopped at a syscall with --stop-at-first-syscall, 1 any other guest stop, 2 usage, I/O,
// disc verification or module validation error. With --json one JSON object is printed on stdout.
#include "dq8/core/error.h"
#include "dq8/core/intake.h"
#include "dq8/core/iso9660.h"
#include "dq8/core/region_profile.h"
#include "dq8/core/version.h"
#include "game/unit_specs.h"
#include "host/json_writer.h"
#include "runtime/ee/context.h"
#include "runtime/ee/dispatcher.h"
#include "runtime/ee/ee_runtime.h"
#include "runtime/ee/elf_loader.h"
#include "runtime/ee/game_module.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/ee/overlay_manager.h"
#include "runtime/kernel/kernel_calls.h"
#include "runtime/machine/machine.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace rt = dq8::rt;
using dq8::core::Error;
using dq8::core::ErrorCode;
using dq8::host::JsonWriter;
using rt::hex32;

constexpr int exit_at_syscall = 0;
constexpr int exit_guest_stop = 1;
constexpr int exit_error = 2;

int usage(std::FILE* out) {
    std::fputs(
        "usage: dq8-game --iso <image> --module-dir <dir> [--regions DIR] [run options]\n"
        "       dq8-game --elf <file> --module-dir <dir> [--window BASE:END] [--resident ID] [run options]\n"
        "\n"
        "  --iso <image>            verify the disc against the region profiles, load the seven unit "
        "modules\n"
        "                           named after the profile and run the boot ELF from its entry\n"
        "  --elf <file>             developer mode: load any MIPS ELF and every dq8_unit_* module in "
        "--module-dir\n"
        "  --module-dir <dir>       where the game modules are (dq8ctl recomp build writes them)\n"
        "  --regions <dir>          region profiles (default: data/regions near the current directory or "
        "the\n"
        "                           executable)\n"
        "  --window BASE:END        developer mode: the overlay window, e.g. 0x00300000:0x00310000\n"
        "  --resident ID            developer mode: the overlay id resident in the window at start\n"
        "\n"
        "run options:\n"
        "  --stop-at-first-syscall  stop at the first kernel call, implemented or not (exit 0 there)\n"
        "  --max-dispatches N       stop after N dispatches (0: no limit)\n"
        "  --trace-dispatch         print every dispatch on stderr\n"
        "  --trap-mmio              stop at the first access to a device register\n"
        "  --json                   print one JSON object on stdout\n"
        "  --version | --help\n"
        "\n"
        "exit status: 0 stopped at a syscall with --stop-at-first-syscall, 1 any other guest stop,\n"
        "2 usage, I/O, disc verification or module validation error\n",
        out);
    return out == stdout ? 0 : exit_error;
}

class UsageError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// Every problem found before the guest could run (module validation, mostly), reported together.
class SetupProblems : public std::runtime_error {
  public:
    SetupProblems(std::string summary, std::vector<std::string> problems)
        : std::runtime_error(std::move(summary)), problems_(std::move(problems)) {}
    const std::vector<std::string>& problems() const noexcept { return problems_; }

  private:
    std::vector<std::string> problems_;
};

struct Options {
    std::optional<fs::path> iso;
    std::optional<fs::path> elf;
    std::optional<fs::path> regions;
    std::optional<fs::path> module_dir;
    std::optional<rt::OverlayWindowSpec> window;
    std::optional<std::uint32_t> resident;
    rt::RunOptions run;
    bool trap_mmio = false;
    bool json = false;
    bool help = false;
    bool version = false;
};

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

Options parse_options(int argc, char** argv) {
    const std::set<std::string, std::less<>> value_options{
        "iso", "elf", "regions", "module-dir", "window", "resident", "max-dispatches"};
    const std::set<std::string, std::less<>> flag_options{
        "stop-at-first-syscall", "trace-dispatch", "trap-mmio", "json", "help", "version"};
    std::map<std::string, std::string, std::less<>> values;
    std::set<std::string, std::less<>> flags;
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
        if (value_options.count(name) != 0) {
            if (!value) {
                if (i + 1 >= argc) {
                    throw UsageError("--" + name + " needs a value");
                }
                value = argv[++i];
            }
            if (!values.emplace(name, *value).second) {
                throw UsageError("--" + name + " given twice");
            }
        } else if (flag_options.count(name) != 0 && !value) {
            flags.insert(name);
        } else {
            throw UsageError("unknown option '" + arg + "'");
        }
    }

    Options o;
    o.help = flags.count("help") != 0;
    o.version = flags.count("version") != 0;
    o.json = flags.count("json") != 0;
    o.trap_mmio = flags.count("trap-mmio") != 0;
    o.run.stop_at_first_syscall = flags.count("stop-at-first-syscall") != 0;
    o.run.trace_dispatch = flags.count("trace-dispatch") != 0;
    if (o.help || o.version) {
        return o;
    }
    const auto get = [&](std::string_view key) -> const std::string* {
        const auto it = values.find(key);
        return it != values.end() ? &it->second : nullptr;
    };
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
    if (const auto* v = get("max-dispatches")) {
        o.run.max_dispatches = parse_number(*v, "--max-dispatches");
    }
    if (const auto* v = get("window")) {
        const std::size_t colon = v->find(':');
        if (colon == std::string::npos) {
            throw UsageError("--window needs BASE:END");
        }
        const std::uint32_t base = parse_u32(std::string_view(*v).substr(0, colon), "--window BASE");
        const std::uint32_t end = parse_u32(std::string_view(*v).substr(colon + 1), "--window END");
        if (base >= end || (base & 3u) != 0 || (end & 3u) != 0) {
            throw UsageError("--window " + *v + ": BASE must be below END and both 4-aligned");
        }
        o.window = rt::OverlayWindowSpec{base, end};
    }
    if (const auto* v = get("resident")) {
        o.resident = parse_u32(*v, "--resident");
    }

    if (o.iso.has_value() == o.elf.has_value()) {
        throw UsageError("give exactly one of --iso and --elf");
    }
    if (!o.module_dir) {
        throw UsageError("--module-dir is required");
    }
    if (o.iso && (o.window || o.resident)) {
        throw UsageError("--window and --resident are developer-mode options (--elf); with --iso the region "
                         "profile gives the window");
    }
    if (o.elf && o.regions) {
        throw UsageError("--regions applies to --iso only");
    }
    if (o.resident && !o.window) {
        throw UsageError("--resident needs --window");
    }
    return o;
}

fs::path executable_dir(const char* argv0) {
    std::error_code ec;
#ifdef __linux__
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return self.parent_path();
    }
#endif
    const fs::path p = fs::canonical(argv0, ec);
    return ec ? fs::path() : p.parent_path();
}

// --regions, else data/regions under the current directory, under an ancestor of the executable (the build
// tree) or share/dq8/regions beside it (an install). Same search as dq8ctl.
fs::path regions_dir(const Options& o, const char* argv0) {
    if (o.regions) {
        return *o.regions;
    }
    std::vector<fs::path> candidates{fs::path("data") / "regions"};
    fs::path dir = executable_dir(argv0);
    for (int up = 0; up < 6 && !dir.empty(); ++up) {
        candidates.push_back(dir / "data" / "regions");
        candidates.push_back(dir / "share" / "dq8" / "regions");
        dir = dir.parent_path();
    }
    std::error_code ec;
    for (const fs::path& c : candidates) {
        if (fs::is_directory(c, ec)) {
            return c;
        }
    }
    throw Error(ErrorCode::io_open, "no region profile directory found; pass --regions DIR");
}

std::vector<std::uint8_t> read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw Error(ErrorCode::io_open, "cannot open " + file.string());
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || size > (std::streamoff{64} << 20)) {
        throw Error(ErrorCode::io_read, file.string() + " is not a readable file of at most 64 MiB");
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw Error(ErrorCode::io_read, "cannot read " + file.string());
    }
    return bytes;
}

// Everything one run needs. Members are destroyed in reverse order, so the modules (units) outlive the
// runtime objects that point into their tables.
struct Session {
    std::string mode; // "iso" or "elf"
    std::optional<dq8::core::VerifiedImage> verified;
    fs::path elf_path;
    std::vector<rt::LoadedUnit> units;
    rt::LoadedElf elf;
    rt::GuestMemory memory;
    rt::OverlayManager overlays;
    std::unique_ptr<rt::Dispatcher> dispatcher;
    std::unique_ptr<rt::Machine> machine;
};

// Registers the validated units: the main unit's table with the dispatcher, overlay tables with the manager.
void install_units(Session& s) {
    s.dispatcher = std::make_unique<rt::Dispatcher>(s.overlays);
    std::vector<std::string> problems;
    for (const rt::LoadedUnit& u : s.units) {
        const dq8_game_module_desc& d = u.module.descriptor();
        try {
            const rt::DispatchTable table = rt::DispatchTable::from_descriptor(d);
            if (d.unit_kind == DQ8_UNIT_MAIN) {
                s.dispatcher->set_main(u.spec.name, table);
            } else {
                s.overlays.register_unit(d.overlay_id, u.spec.name, table);
            }
        } catch (const std::invalid_argument& e) {
            problems.push_back(u.spec.name + ": " + e.what());
        }
    }
    if (!problems.empty()) {
        throw SetupProblems("the game modules do not fit together", std::move(problems));
    }
}

void setup_iso(const Options& o, const char* argv0, Session& s) {
    s.mode = "iso";
    const std::vector<dq8::core::RegionProfile> profiles =
        dq8::core::load_region_profiles(regions_dir(o, argv0));
    s.verified = dq8::core::verify_image(*o.iso, profiles);
    const dq8::core::RegionProfile& profile = s.verified->profile;

    const std::vector<rt::UnitSpec> specs = dq8::game::unit_specs(profile);
    rt::ModuleSet set = rt::load_modules(specs, *o.module_dir);
    if (!set.ok()) {
        const std::string summary = std::to_string(set.problems.size()) +
                                    " problem(s) with the game modules in " + o.module_dir->string() +
                                    " for " + profile.id;
        throw SetupProblems(summary, std::move(set.problems));
    }
    s.units = std::move(set.units);
    s.overlays = rt::OverlayManager(dq8::game::overlay_window_spec(profile));
    install_units(s);

    const dq8::core::iso::Image image = dq8::core::iso::Image::open(*o.iso);
    const std::vector<std::uint8_t> boot = image.read_all(image.require(s.verified->boot_path));
    s.elf = rt::load_elf(boot, s.memory);
}

void setup_elf(const Options& o, Session& s) {
    s.mode = "elf";
    s.elf_path = *o.elf;
    s.elf = rt::load_elf(read_file(*o.elf), s.memory);
    if (o.window) {
        s.overlays = rt::OverlayManager(*o.window);
    }

    std::vector<std::string> problems;
    const std::vector<fs::path> files = rt::find_module_files(*o.module_dir);
    if (files.empty()) {
        problems.push_back("no unit modules (" + rt::shared_library_file_name("dq8_unit_<name>") + ") in " +
                           o.module_dir->string());
    }
    std::size_t mains = 0;
    for (const fs::path& file : files) {
        const std::string unit = rt::unit_name_from_module_file(file);
        try {
            rt::GameModule module = rt::GameModule::open(file);
            const dq8_game_module_desc& d = module.descriptor();
            // Developer mode has no profile: the descriptor says what the unit is, and only its own
            // consistency, its name, the ELF entry (main) and the window (overlays) are checked.
            rt::UnitSpec spec;
            spec.name = unit;
            const bool readable = d.magic == DQ8_GAME_MODULE_MAGIC &&
                                  d.abi_version == DQ8_GAME_MODULE_ABI_VERSION &&
                                  d.struct_size == sizeof(dq8_game_module_desc);
            spec.kind = readable && d.unit_kind == DQ8_UNIT_OVERLAY ? DQ8_UNIT_OVERLAY : DQ8_UNIT_MAIN;
            spec.overlay_id = readable && spec.kind == DQ8_UNIT_OVERLAY ? d.overlay_id : 0;
            if (spec.kind == DQ8_UNIT_MAIN) {
                spec.entry_point = s.elf.entry;
                ++mains;
            } else if (o.window) {
                spec.range_begin = o.window->base;
                spec.range_end = o.window->end;
            } else {
                problems.push_back(unit + ": an overlay unit needs --window");
            }
            const std::vector<std::string> found = rt::validate_descriptor(&d, spec);
            for (const std::string& p : found) {
                problems.push_back(unit + ": " + p + " (" + file.filename().string() + ")");
            }
            if (found.empty()) {
                s.units.push_back({spec, std::move(module)});
            }
        } catch (const std::exception& e) {
            problems.push_back(unit + ": " + e.what());
        }
    }
    if (!files.empty() && mains != 1) {
        problems.push_back(std::to_string(mains) + " main units in " + o.module_dir->string() +
                           ", expected exactly one");
    }
    if (!problems.empty()) {
        const std::string summary = std::to_string(problems.size()) +
                                    " problem(s) with the game modules in " + o.module_dir->string();
        throw SetupProblems(summary, std::move(problems));
    }
    install_units(s);
    if (o.window) {
        const rt::DispatchTable& main = s.dispatcher->main_table();
        if (main.base() < o.window->end && o.window->base < main.end()) {
            throw SetupProblems("the main unit overlaps the overlay window",
                                {"main table [" + hex32(main.base()) + ", " + hex32(main.end()) +
                                 ") overlaps --window [" + hex32(o.window->base) + ", " +
                                 hex32(o.window->end) + ")"});
        }
    }
    if (o.resident) {
        if (s.overlays.find(*o.resident) == nullptr) {
            throw UsageError("--resident " + std::to_string(*o.resident) + ": no overlay unit has that id");
        }
        s.overlays.set_resident(*o.resident);
    }
}

std::string hex64(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%016llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string mmio_access_name(const rt::MmioAccess& a) {
    return std::string(a.store ? "store" : "load") + std::to_string(a.width * 8u);
}

bool stopped_at_syscall(const rt::StopInfo& stop) {
    return stop.reason == rt::StopReason::syscall ||
           (stop.reason == rt::StopReason::named_call && stop.call_kind == DQ8_CALL_SYSCALL);
}

void json_modules(JsonWriter& j, const Session& s) {
    j.key("modules");
    j.begin_array();
    for (const rt::LoadedUnit& u : s.units) {
        const dq8_game_module_desc& d = u.module.descriptor();
        const rt::DispatchTable table = rt::DispatchTable::from_descriptor(d);
        j.begin_object();
        j.member("unit", u.spec.name);
        j.member("path", u.module.path().string());
        j.member("kind", d.unit_kind == DQ8_UNIT_MAIN ? "main" : "overlay");
        j.member("overlay_id", d.overlay_id);
        j.member("profile_id", rt::descriptor_string(d.profile_id));
        j.member("source_sha1", rt::descriptor_string(d.source_sha1));
        j.member("generator", rt::descriptor_string(d.generator));
        j.member("entry_point", hex32(d.entry_point));
        j.member("table_base", hex32(d.table_base));
        j.member("table_end", hex32(d.table_end));
        j.member("slots", d.table_slot_count);
        j.member("entries", table.entry_count());
        j.end_object();
    }
    j.end_array();
}

void json_stop(JsonWriter& j, const rt::StopInfo& stop) {
    j.key("stop");
    j.begin_object();
    j.member("reason", rt::to_string(stop.reason));
    j.member("message", stop.message);
    j.member("pc", hex32(stop.pc));
    j.member("address", hex32(stop.address));
    if (stop.source != 0) {
        j.member("source", hex32(stop.source));
    }
    if (stop.reason == rt::StopReason::syscall || stop.reason == rt::StopReason::named_call) {
        j.member("call_kind", rt::call_kind_name(stop.call_kind));
        j.member("name", stop.call_name);
        if (stop.has_number) {
            j.member("number", rt::format_kernel_call_number(stop.number));
        } else {
            j.key("number");
            j.null();
        }
        j.key("args");
        j.begin_object();
        for (std::size_t i = 0; i < stop.args.size(); ++i) {
            j.member("a" + std::to_string(i), hex32(stop.args[i]));
        }
        j.end_object();
    }
    if (stop.reason == rt::StopReason::guest_exception) {
        j.member("exception", stop.number);
    }
    if (!stop.last_branch.empty()) {
        j.key("last_branch");
        j.begin_object();
        j.member("kind", stop.last_branch);
        if (stop.last_branch != "run-loop") {
            j.member("source", hex32(stop.last_source));
        }
        j.member("target", hex32(stop.last_target));
        j.end_object();
    }
    j.end_object();
}

void json_context(JsonWriter& j, const R5900Context& ctx) {
    j.key("context");
    j.begin_object();
    j.member("pc", hex32(ctx.pc));
    j.member("hi", hex64(ctx.hi));
    j.member("lo", hex64(ctx.lo));
    j.member("sa", hex32(ctx.sa));
    j.key("gpr");
    j.begin_object();
    for (unsigned r = 0; r < 32; ++r) {
        j.member(rt::gpr_names[r], hex64(rt::gpr_lo(ctx, r)));
    }
    j.end_object();
    j.key("gpr_hi");
    j.begin_object();
    for (unsigned r = 0; r < 32; ++r) {
        if (rt::gpr_hi(ctx, r) != 0) {
            j.member(rt::gpr_names[r], hex64(rt::gpr_hi(ctx, r)));
        }
    }
    j.end_object();
    j.end_object();
}

void json_mmio(JsonWriter& j, const rt::GuestMemory& memory) {
    j.key("mmio");
    j.begin_array();
    for (const rt::MmioAccess& a : memory.mmio_log()) {
        j.begin_object();
        j.member("address", hex32(a.address));
        j.member("region", rt::to_string(a.region));
        j.member("first_access", mmio_access_name(a));
        if (a.store) {
            j.member("value", hex64(a.value));
        }
        j.member("pc", hex32(a.pc));
        j.member("count", a.count);
        j.end_object();
    }
    j.end_array();
}

void json_setup(JsonWriter& j, const Session& s) {
    j.member("mode", s.mode);
    if (s.verified) {
        const dq8::core::RegionProfile& p = s.verified->profile;
        j.key("profile");
        j.begin_object();
        j.member("id", p.id);
        j.member("title", p.title);
        j.member("boot_path", s.verified->boot_path);
        j.member("boot_sha1", s.verified->boot.sha1);
        j.end_object();
    } else {
        j.member("elf_path", s.elf_path.string());
    }
    j.key("elf");
    j.begin_object();
    j.member("entry", hex32(s.elf.entry));
    j.member("segments", s.elf.segments.size());
    j.end_object();
    if (const auto& w = s.overlays.window()) {
        j.key("overlay_window");
        j.begin_object();
        j.member("base", hex32(w->base));
        j.member("end", hex32(w->end));
        j.end_object();
    }
    j.key("resident");
    if (const rt::OverlayUnit* r = s.overlays.resident()) {
        j.value(r->overlay_id);
    } else {
        j.null();
    }
    json_modules(j, s);
}

void print_setup(const Session& s) {
    if (s.verified) {
        const dq8::core::RegionProfile& p = s.verified->profile;
        std::printf("disc: %s (%s), boot %s verified\n", p.id.c_str(), p.title.c_str(),
                    s.verified->boot_path.c_str());
    } else {
        std::printf("elf: %s\n", s.elf_path.string().c_str());
    }
    for (const rt::LoadedUnit& u : s.units) {
        const dq8_game_module_desc& d = u.module.descriptor();
        const rt::DispatchTable table = rt::DispatchTable::from_descriptor(d);
        std::printf("module %-8s %s  [%s, %s)  %zu entries  %s\n", u.spec.name.c_str(),
                    u.module.path().filename().string().c_str(), hex32(d.table_base).c_str(),
                    hex32(d.table_end).c_str(), table.entry_count(),
                    std::string(rt::descriptor_string(d.generator)).c_str());
    }
    if (const auto& w = s.overlays.window()) {
        const rt::OverlayUnit* r = s.overlays.resident();
        std::printf("overlay window [%s, %s), resident: %s\n", hex32(w->base).c_str(), hex32(w->end).c_str(),
                    r != nullptr ? r->name.c_str() : "none");
    }
    std::printf("loaded %zu segments, entry %s\n", s.elf.segments.size(), hex32(s.elf.entry).c_str());
}

void print_result(const Session& s, const rt::RunResult& r) {
    std::printf("stopped after %llu dispatches: %s: %s\n", static_cast<unsigned long long>(r.dispatches),
                std::string(rt::to_string(r.stop.reason)).c_str(), r.stop.message.c_str());
    if (r.stop.last_branch == "run-loop") {
        std::printf("last dispatch: run loop -> %s\n", hex32(r.stop.last_target).c_str());
    } else if (!r.stop.last_branch.empty()) {
        std::printf("last dispatch: %s %s -> %s\n", r.stop.last_branch.c_str(),
                    hex32(r.stop.last_source).c_str(), hex32(r.stop.last_target).c_str());
    }
    std::fputs(rt::format_context(s.machine->ee().context()).c_str(), stdout);
    const auto& log = s.memory.mmio_log();
    std::printf("mmio: %zu addresses, %llu accesses\n", log.size(),
                static_cast<unsigned long long>(s.memory.mmio_access_count()));
    for (const rt::MmioAccess& a : log) {
        std::printf("  %-8s %s (%s) first at pc %s, %llu accesses\n", mmio_access_name(a).c_str(),
                    hex32(a.address).c_str(), std::string(rt::to_string(a.region)).c_str(),
                    hex32(a.pc).c_str(), static_cast<unsigned long long>(a.count));
    }
}

int report_failure(bool json, std::string_view code, const std::string& message,
                   const std::vector<std::string>& problems = {}) {
    if (json) {
        JsonWriter j;
        j.begin_object();
        j.member("ok", false);
        j.key("error");
        j.begin_object();
        j.member("code", code);
        j.member("message", message);
        j.end_object();
        j.key("problems");
        j.begin_array();
        for (const std::string& p : problems) {
            j.value(p);
        }
        j.end_array();
        j.member("exit_code", exit_error);
        j.end_object();
        std::printf("%s\n", j.str().c_str());
    } else {
        std::fprintf(stderr, "dq8-game: %s\n", message.c_str());
        for (const std::string& p : problems) {
            std::fprintf(stderr, "  %s\n", p.c_str());
        }
    }
    return exit_error;
}

int run_game(const Options& o, const char* argv0) {
    Session s;
    if (o.iso) {
        setup_iso(o, argv0, s);
    } else {
        setup_elf(o, s);
    }
    s.memory.set_trap_mmio(o.trap_mmio);
    if (!o.json) {
        s.memory.set_mmio_sink([](const rt::MmioAccess& a) {
            std::fprintf(stderr, "mmio: first %s at %s (%s), pc %s\n", mmio_access_name(a).c_str(),
                         hex32(a.address).c_str(), std::string(rt::to_string(a.region)).c_str(),
                         hex32(a.pc).c_str());
        });
        print_setup(s);
        std::fflush(stdout); // before the run's stderr diagnostics
    }
    rt::MachineConfig config;
    config.stop_at_first_syscall = o.run.stop_at_first_syscall;
    config.max_call_depth = o.run.max_call_depth;
    config.trace_dispatch = o.run.trace_dispatch;
    s.machine = std::make_unique<rt::Machine>(s.memory, *s.dispatcher, config);
    s.machine->boot(s.elf.entry);
    rt::RunLimits limits;
    limits.max_dispatches = o.run.max_dispatches;
    const rt::SliceResult slice = s.machine->run(limits);
    const rt::RunResult result{slice.stop.value_or(rt::StopInfo{}), slice.dispatches};
    const int code =
        stopped_at_syscall(result.stop) && o.run.stop_at_first_syscall ? exit_at_syscall : exit_guest_stop;
    if (o.json) {
        JsonWriter j;
        j.begin_object();
        j.member("ok", code == exit_at_syscall);
        json_setup(j, s);
        json_stop(j, result.stop);
        j.member("dispatches", result.dispatches);
        json_context(j, s.machine->ee().context());
        json_mmio(j, s.memory);
        j.member("exit_code", code);
        j.end_object();
        std::printf("%s\n", j.str().c_str());
    } else {
        print_result(s, result);
    }
    return code;
}

} // namespace

int main(int argc, char** argv) {
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        json = json || std::string_view(argv[i]) == "--json";
    }
    try {
        const Options o = parse_options(argc, argv);
        if (o.help) {
            return usage(stdout);
        }
        if (o.version) {
            std::printf("dq8-game %s\n", std::string(dq8::core::version()).c_str());
            return 0;
        }
        return run_game(o, argv[0]);
    } catch (const UsageError& e) {
        if (!json) {
            std::fprintf(stderr, "dq8-game: %s\n\n", e.what());
            usage(stderr);
            return exit_error;
        }
        return report_failure(json, "usage", e.what());
    } catch (const SetupProblems& e) {
        return report_failure(json, "module-validation", e.what(), e.problems());
    } catch (const Error& e) {
        return report_failure(json, dq8::core::to_string(e.code()), e.what());
    } catch (const std::exception& e) {
        return report_failure(json, "error", e.what());
    }
}
