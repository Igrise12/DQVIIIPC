// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "host/session.h"

#include "dq8/core/error.h"
#include "dq8/core/iso9660.h"
#include "dq8/core/region_profile.h"
#include "game/unit_specs.h"
#include "runtime/ee/context.h"
#include "runtime/ee/dispatch_table.h"
#include "runtime/ee/ee_runtime.h"
#include "runtime/kernel/kernel_calls.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <string_view>
#include <utility>

namespace dq8::host {

namespace {

using core::Error;
using core::ErrorCode;
using rt::hex32;

} // namespace

fs::path executable_dir(const char* argv0) {
    std::error_code ec;
#ifdef __linux__
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return self.parent_path();
    }
#elif defined(_WIN32)
    // The CRT's full path of this executable: argv[0] may be a bare name typed at a prompt (M5 session E).
    char* self = nullptr;
    if (_get_pgmptr(&self) == 0 && self != nullptr && self[0] != '\0') {
        return fs::path(self).parent_path();
    }
#endif
    const fs::path p = fs::canonical(argv0, ec);
    return ec ? fs::path() : p.parent_path();
}

namespace {

// --regions, else data/regions under the current directory, under an ancestor of the executable (the build
// tree) or share/dq8/regions beside it (an install). Same search as dq8ctl.
fs::path regions_dir(const Options& o, const char* argv0) {
    if (o.regions) {
        return *o.regions;
    }
    std::vector<fs::path> candidates{fs::path("data") / "regions"};
    fs::path dir = executable_dir(argv0);
    for (int up = 0; up < 8 && !dir.empty(); ++up) { // 8: a multi-config build adds Release/ or Debug/
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
    const std::vector<core::RegionProfile> profiles = core::load_region_profiles(regions_dir(o, argv0));
    s.verified = core::verify_image(*o.iso, profiles);
    const core::RegionProfile& profile = s.verified->profile;

    const std::vector<rt::UnitSpec> specs = game::unit_specs(profile);
    rt::ModuleSet set = rt::load_modules(specs, *o.module_dir);
    if (!set.ok()) {
        const std::string summary = std::to_string(set.problems.size()) +
                                    " problem(s) with the game modules in " + o.module_dir->string() +
                                    " for " + profile.id;
        throw SetupProblems(summary, std::move(set.problems));
    }
    s.units = std::move(set.units);
    s.overlays = rt::OverlayManager(game::overlay_window_spec(profile));
    install_units(s);

    const core::iso::Image image = core::iso::Image::open(*o.iso);
    const std::vector<std::uint8_t> boot = image.read_all(image.require(s.verified->boot_path));
    s.elf = rt::load_elf(boot, s.memory);
}

void setup_elf(const Options& o, Session& s) {
    s.mode = "elf";
    s.elf_path = *o.elf;
    s.elf = rt::load_elf(read_file(*o.elf), s.memory);
    if (o.overlay_window) {
        s.overlays = rt::OverlayManager(*o.overlay_window);
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
            } else if (o.overlay_window) {
                spec.range_begin = o.overlay_window->base;
                spec.range_end = o.overlay_window->end;
            } else {
                problems.push_back(unit + ": an overlay unit needs --overlay-window");
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
    if (o.overlay_window) {
        const rt::DispatchTable& main = s.dispatcher->main_table();
        if (main.base() < o.overlay_window->end && o.overlay_window->base < main.end()) {
            throw SetupProblems("the main unit overlaps the overlay window",
                                {"main table [" + hex32(main.base()) + ", " + hex32(main.end()) +
                                 ") overlaps --overlay-window [" + hex32(o.overlay_window->base) + ", " +
                                 hex32(o.overlay_window->end) + ")"});
        }
    }
    if (o.resident) {
        if (s.overlays.find(*o.resident) == nullptr) {
            throw UsageError("--resident " + std::to_string(*o.resident) + ": no overlay unit has that id");
        }
        s.overlays.set_resident(*o.resident);
    }
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

} // namespace

void setup_session(const Options& o, const char* argv0, Session& s) {
    if (o.iso) {
        setup_iso(o, argv0, s);
    } else {
        setup_elf(o, s);
    }
}

bool stopped_at_syscall(const rt::StopInfo& stop) {
    return stop.reason == rt::StopReason::syscall ||
           (stop.reason == rt::StopReason::named_call && stop.call_kind == DQ8_CALL_SYSCALL);
}

std::string hex64(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%016llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string mmio_access_name(const rt::MmioAccess& a) {
    return std::string(a.store ? "store" : "load") + std::to_string(a.width * 8u);
}

void json_setup(JsonWriter& j, const Session& s) {
    j.member("mode", s.mode);
    if (s.verified) {
        const core::RegionProfile& p = s.verified->profile;
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

void json_stop(JsonWriter& j, const rt::StopInfo& stop) {
    j.key("stop");
    j.begin_object();
    j.member("reason", rt::to_string(stop.reason));
    j.member("message", stop.message);
    j.member("pc", hex32(stop.pc));
    j.member("address", hex32(stop.address));
    // The thread that was running: its id, 0 for the scheduler, negative for the interrupt handler fiber.
    j.member("thread_id", stop.thread_id);
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

void print_setup(const Session& s) {
    if (s.verified) {
        const core::RegionProfile& p = s.verified->profile;
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

void print_stop(const rt::StopInfo& stop, std::uint64_t dispatches, const R5900Context& ctx,
                const rt::GuestMemory& memory) {
    std::printf("stopped after %llu dispatches: %s: %s\n", static_cast<unsigned long long>(dispatches),
                std::string(rt::to_string(stop.reason)).c_str(), stop.message.c_str());
    if (stop.last_branch == "run-loop") {
        std::printf("last dispatch: run loop -> %s\n", hex32(stop.last_target).c_str());
    } else if (!stop.last_branch.empty()) {
        std::printf("last dispatch: %s %s -> %s\n", stop.last_branch.c_str(), hex32(stop.last_source).c_str(),
                    hex32(stop.last_target).c_str());
    }
    std::fputs(rt::format_context(ctx).c_str(), stdout);
    const auto& log = memory.mmio_log();
    std::printf("mmio: %zu addresses, %llu accesses\n", log.size(),
                static_cast<unsigned long long>(memory.mmio_access_count()));
    for (const rt::MmioAccess& a : log) {
        std::printf("  %-8s %s (%s) first at pc %s, %llu accesses\n", mmio_access_name(a).c_str(),
                    hex32(a.address).c_str(), std::string(rt::to_string(a.region)).c_str(),
                    hex32(a.pc).c_str(), static_cast<unsigned long long>(a.count));
    }
}

int report_failure(bool json, std::string_view code, const std::string& message,
                   const std::vector<std::string>& problems, int exit_code) {
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
        j.member("exit_code", exit_code);
        j.end_object();
        std::printf("%s\n", j.str().c_str());
    } else {
        std::fprintf(stderr, "dq8-game: %s\n", message.c_str());
        for (const std::string& p : problems) {
            std::fprintf(stderr, "  %s\n", p.c_str());
        }
    }
    return exit_code;
}

} // namespace dq8::host
