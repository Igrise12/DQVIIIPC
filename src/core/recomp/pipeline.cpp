// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/recomp/pipeline.h"

#include "dq8/core/error.h"
#include "dq8/core/hash.h"
#include "dq8/core/intake.h"
#include "dq8/core/iso9660.h"
#include "dq8/core/overlay_elf.h"
#include "dq8/core/recomp/function_map.h"
#include "dq8/core/recomp/recomp_config.h"
#include "dq8/core/version.h"
#include "platform/files.h"
#include "recomp/common.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <sstream>
#include <toml.hpp>

namespace dq8::core::recomp {

namespace fs = std::filesystem;
using detail::hex8;
using detail::path_utf8;

namespace {

constexpr int units_schema_version = 1;
constexpr int manifest_schema_version = 1;
constexpr std::uint64_t max_unit_source = 64u << 20;

// --- small TOML helpers (toml11 stays private to this library) ----------------------------------------

toml::ordered_value hex_value(std::uint32_t v) {
    toml::ordered_value value(static_cast<std::int64_t>(v));
    value.as_integer_fmt().fmt = toml::integer_format::hex;
    value.as_integer_fmt().width = 8;
    return value;
}

std::string header_comment(const std::string& what) {
    return "# " + what +
           "\n# Everything here is derived from your disc (hashes, addresses): keep it local, never commit "
           "it.\n";
}

toml::ordered_value read_toml(const fs::path& file) {
    std::istringstream stream(platform::read_text_file(file));
    auto parsed = toml::try_parse<toml::ordered_type_config>(stream, path_utf8(file));
    if (parsed.is_err()) {
        throw Error(ErrorCode::work_stale, "'" + path_utf8(file) +
                                               "' is not valid TOML; rerun the stage that "
                                               "writes it");
    }
    return std::move(parsed.unwrap());
}

std::string str_of(const toml::ordered_value& t, const char* key) {
    return t.contains(key) && t.at(key).is_string() ? t.at(key).as_string() : std::string();
}

std::int64_t int_of(const toml::ordered_value& t, const char* key) {
    return t.contains(key) && t.at(key).is_integer() ? t.at(key).as_integer() : 0;
}

std::string utc_timestamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
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

double seconds(std::chrono::milliseconds ms) {
    return static_cast<double>(ms.count()) / 1000.0;
}

void progress(const PipelineOptions& o, const std::string& unit, const std::string& message) {
    if (o.on_progress) {
        o.on_progress(unit, message);
    }
}

void check_cancel(const PipelineOptions& o) {
    if (o.cancel != nullptr && o.cancel->load()) {
        throw Error(ErrorCode::process_cancelled, "cancelled");
    }
}

std::string sha1_text(const std::string& text) {
    return to_hex(Sha1::of(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
}

// --- units/units.toml -------------------------------------------------------------------------------------

struct UnitRecord {
    std::string name;
    UnitKind kind = UnitKind::main;
    std::uint32_t overlay_id = 0;
    std::string source_path;
    std::string source_sha1;
    std::uint32_t entry = 0;
    fs::path elf;
    fs::path config;
    fs::path map;
};

struct UnitsFile {
    std::string profile;
    std::string boot_sha1;
    std::vector<UnitRecord> units;

    const UnitRecord* find(std::string_view name) const {
        for (const UnitRecord& u : units) {
            if (u.name == name) {
                return &u;
            }
        }
        return nullptr;
    }
};

UnitsFile read_units_file(const WorkLayout& layout, const VerifiedImage& v) {
    const fs::path file = layout.units_file();
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        throw Error(ErrorCode::work_stale,
                    "'" + path_utf8(file) + "' does not exist; run `dq8ctl recomp prepare` first");
    }
    const toml::ordered_value root = read_toml(file);
    if (int_of(root, "schema_version") != units_schema_version) {
        throw Error(ErrorCode::work_stale,
                    "'" + path_utf8(file) + "' has another schema version; rerun prepare");
    }
    UnitsFile f;
    f.profile = str_of(root, "profile");
    f.boot_sha1 = str_of(root, "boot_sha1");
    if (f.profile != v.profile.id || f.boot_sha1 != v.boot.sha1) {
        throw Error(ErrorCode::work_stale, "the work directory '" + path_utf8(layout.root()) +
                                               "' was prepared for another disc (profile '" + f.profile +
                                               "'); run `dq8ctl recomp prepare` for this one");
    }
    if (root.contains("units") && root.at("units").is_array()) {
        for (const toml::ordered_value& t : root.at("units").as_array()) {
            UnitRecord r;
            r.name = str_of(t, "name");
            r.kind = str_of(t, "kind") == "overlay" ? UnitKind::overlay : UnitKind::main;
            r.overlay_id = static_cast<std::uint32_t>(int_of(t, "overlay_id"));
            r.source_path = str_of(t, "source_path");
            r.source_sha1 = str_of(t, "source_sha1");
            r.entry = static_cast<std::uint32_t>(int_of(t, "entry"));
            r.elf = fs::path(str_of(t, "elf"));
            r.config = fs::path(str_of(t, "config"));
            r.map = fs::path(str_of(t, "map"));
            f.units.push_back(std::move(r));
        }
    }
    return f;
}

// Writes units.toml with this run's units plus the earlier records of units this run did not touch.
void write_units_file(const WorkLayout& layout, const VerifiedImage& v, const std::vector<CodeUnit>& order,
                      const std::vector<PreparedUnit>& prepared) {
    std::map<std::string, toml::ordered_value> rows;
    std::error_code ec;
    if (fs::is_regular_file(layout.units_file(), ec)) {
        try {
            const toml::ordered_value old = read_toml(layout.units_file());
            if (str_of(old, "profile") == v.profile.id && str_of(old, "boot_sha1") == v.boot.sha1 &&
                old.contains("units") && old.at("units").is_array()) {
                for (const toml::ordered_value& t : old.at("units").as_array()) {
                    rows[str_of(t, "name")] = t;
                }
            }
        } catch (const Error&) {
            // A damaged record only costs re-preparing the units it described.
        }
    }
    for (const PreparedUnit& p : prepared) {
        toml::ordered_value t(toml::ordered_table{});
        t["name"] = p.unit.name;
        t["kind"] = std::string(to_string(p.unit.kind));
        t["overlay_id"] = static_cast<std::int64_t>(p.unit.overlay_id);
        t["source_path"] = p.unit.source_path;
        t["source_sha1"] = p.source_sha1;
        t["entry"] = hex_value(p.unit.entry);
        t["code_start"] = hex_value(p.unit.code_start);
        t["code_end"] = hex_value(p.unit.code_end);
        t["elf"] = path_utf8(p.elf);
        t["config"] = path_utf8(p.config);
        t["map"] = path_utf8(p.map_csv);
        t["functions"] = static_cast<std::int64_t>(p.functions);
        t["stubs"] = static_cast<std::int64_t>(p.stubs);
        t["entry_points"] = static_cast<std::int64_t>(p.entry_points);
        t["jump_tables"] = static_cast<std::int64_t>(p.jump_tables);
        rows[p.unit.name] = std::move(t);
    }
    toml::ordered_value units(toml::ordered_array{});
    for (const CodeUnit& u : order) {
        if (const auto it = rows.find(u.name); it != rows.end()) {
            units.as_array().push_back(it->second);
        }
    }
    units.as_array_fmt().fmt = toml::array_format::array_of_tables;
    toml::ordered_value root(toml::ordered_table{});
    root["schema_version"] = units_schema_version;
    root["profile"] = v.profile.id;
    root["boot_sha1"] = v.boot.sha1;
    root["tool_version"] = std::string(version());
    root["units"] = std::move(units);
    platform::write_file_if_changed(layout.units_file(),
                                    header_comment("Code units written by `dq8ctl recomp prepare`.") +
                                        toml::format(root));
}

// --- stamps/<unit>.generate.toml ---------------------------------------------------------------------------

struct GenerateRecord {
    std::string key;
    std::string generator;
    GenerateResult result;
};

std::optional<GenerateRecord> read_generate_record(const fs::path& file) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        return std::nullopt;
    }
    try {
        const toml::ordered_value root = read_toml(file);
        GenerateRecord r;
        r.key = str_of(root, "key");
        r.generator = str_of(root, "generator");
        r.result.elapsed = std::chrono::milliseconds(int_of(root, "elapsed_ms"));
        r.result.files = static_cast<std::size_t>(int_of(root, "files"));
        r.result.bytes = static_cast<std::uint64_t>(int_of(root, "bytes"));
        r.result.report.found = true;
        if (root.contains("counts") && root.at("counts").is_table()) {
            for (const auto& [k, v] : root.at("counts").as_table()) {
                if (v.is_integer()) {
                    r.result.report.counts.emplace_back(k, static_cast<std::uint64_t>(v.as_integer()));
                }
            }
        }
        return r;
    } catch (const std::exception&) {
        return std::nullopt; // a damaged record means "run again"
    }
}

void write_generate_record(const fs::path& file, const GenerateRecord& r) {
    toml::ordered_value counts(toml::ordered_table{});
    for (const auto& [k, v] : r.result.report.counts) {
        counts[k] = static_cast<std::int64_t>(v);
    }
    toml::ordered_value root(toml::ordered_table{});
    root["key"] = r.key;
    root["generator"] = r.generator;
    root["elapsed_ms"] = static_cast<std::int64_t>(r.result.elapsed.count());
    root["files"] = static_cast<std::int64_t>(r.result.files);
    root["bytes"] = static_cast<std::int64_t>(r.result.bytes);
    root["counts"] = std::move(counts);
    platform::write_file_atomic(file,
                                header_comment("Written by `dq8ctl recomp generate`: the input key of the "
                                               "last successful run and its report counts.") +
                                    toml::format(root));
}

std::string generate_key(const std::string& generator, const UnitRecord& u) {
    return sha1_text("generator=" + generator + "\nconfig=" + platform::sha1_file(u.config) +
                     "\nelf=" + platform::sha1_file(u.elf) + "\nmap=" + platform::sha1_file(u.map) + "\n");
}

// A digest of a generated tree: SHA-1 over each regular file's relative path and SHA-1, in path order.
std::string tree_sha1(const fs::path& dir, std::size_t& files, std::uint64_t& bytes) {
    std::vector<fs::path> paths;
    for (const fs::directory_entry& e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file()) {
            paths.push_back(e.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    std::string listing;
    files = paths.size();
    bytes = 0;
    for (const fs::path& p : paths) {
        bytes += fs::file_size(p);
        const std::u8string rel = fs::relative(p, dir).generic_u8string();
        listing += std::string(rel.begin(), rel.end()) + " " + platform::sha1_file(p) + "\n";
    }
    return sha1_text(listing);
}

bool copy_if_changed(const fs::path& from, const fs::path& to) {
    if (platform::same_contents(from, to)) {
        return false;
    }
    platform::make_directories(to.parent_path());
    const fs::path tmp = to.string() + ".tmp";
    std::error_code ec;
    fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec);
    if (!ec) {
        fs::rename(tmp, to, ec);
    }
    if (ec) {
        throw Error(ErrorCode::io_write,
                    "cannot copy '" + path_utf8(from) + "' to '" + path_utf8(to) + "': " + ec.message());
    }
    return true;
}

void ensure_gitignore(const fs::path& root) {
    platform::write_file_if_changed(root / ".gitignore", std::string_view("*\n"));
}

} // namespace

PrepareReport prepare_units(const VerifiedImage& v, const PipelineOptions& o) {
    if (o.maps_dir.empty()) {
        throw Error(ErrorCode::invalid_argument, "prepare needs the function-map directory (--maps DIR)");
    }
    const WorkLayout layout(o.work_dir);
    const std::vector<CodeUnit> all = code_units(v);
    const std::vector<CodeUnit> selected = select_units(all, o.units);
    platform::make_directories(layout.root());
    ensure_gitignore(layout.root());
    const iso::Image image = iso::Image::open(v.image_path);
    const Mwo3Window window{v.profile.overlay_window.base, v.profile.overlay_window.end};

    PrepareReport report;
    report.profile_id = v.profile.id;
    report.units_file = layout.units_file();
    for (const CodeUnit& unit : selected) {
        check_cancel(o);
        progress(o, unit.name, "reading " + unit.source_path + " from the disc");
        PreparedUnit p;
        p.unit = unit;
        const std::vector<std::uint8_t> source =
            image.read_all(image.require(unit.source_path), max_unit_source);
        p.source_sha1 = to_hex(Sha1::of(source));
        std::vector<std::uint8_t> elf;
        if (unit.kind == UnitKind::main) {
            if (p.source_sha1 != v.boot.sha1) {
                throw Error(ErrorCode::image_mismatch, "the boot file read back with SHA-1 " + p.source_sha1 +
                                                           ", not the verified " + v.boot.sha1);
            }
            elf = source; // the boot ELF is its own unit ELF
        } else {
            elf = make_overlay_unit_elf(source, v.elf.flags, window).elf;
        }
        p.elf = fs::absolute(layout.unit_elf(unit.name));
        p.elf_written = platform::write_file_if_changed(p.elf, elf);

        progress(o, unit.name, "reading the function map");
        FunctionMap map = load_function_map(o.maps_dir, unit.name);
        p.warnings = validate_function_map(map, unit.code_start, unit.code_end);
        p.map_csv = fs::absolute(map.csv_path);
        p.functions = map.functions.size();
        p.jump_tables = map.jump_tables.size();

        RecompConfigInput in;
        in.unit = unit.name;
        in.profile_id = v.profile.id;
        in.elf = p.elf;
        in.output_dir = fs::absolute(layout.generated_dir(unit.name));
        in.map_csv = p.map_csv;
        in.map = &map;
        const RecompConfig config = make_recomp_config(in);
        p.stubs = config.stubs.size();
        p.entry_points = config.entry_points.size();
        p.config = fs::absolute(layout.unit_config(unit.name));
        p.config_written = platform::write_file_if_changed(p.config, config.toml);
        progress(o, unit.name,
                 std::to_string(p.functions) + " functions, " + std::to_string(p.stubs) + " stub bindings, " +
                     std::to_string(p.entry_points) + " entry points, " + std::to_string(p.jump_tables) +
                     " jump tables");
        report.units.push_back(std::move(p));
    }
    write_units_file(layout, v, all, report.units);
    return report;
}

GenerateReport generate_units(const VerifiedImage& v, const PipelineOptions& o) {
    const WorkLayout layout(o.work_dir);
    const std::vector<CodeUnit> selected = select_units(code_units(v), o.units);
    const UnitsFile units = read_units_file(layout, v);
    if (o.ps2_recomp.empty()) {
        throw Error(ErrorCode::tool_missing, "generate needs the recompiler (--ps2-recomp PATH)");
    }
    const std::optional<fs::path> exe = find_program(o.ps2_recomp);
    if (!exe) {
        throw Error(ErrorCode::tool_missing, "cannot find the recompiler '" + path_utf8(o.ps2_recomp) + "'");
    }
    GenerateReport report;
    report.ps2_recomp = *exe;
    report.generator_id = "ps2_recomp sha1:" + platform::sha1_file(*exe).substr(0, 12);

    for (const CodeUnit& unit : selected) {
        check_cancel(o);
        const UnitRecord* rec = units.find(unit.name);
        if (rec == nullptr) {
            throw Error(ErrorCode::work_stale,
                        "unit '" + unit.name + "' has not been prepared; run `dq8ctl recomp prepare` first");
        }
        GeneratedUnit g;
        g.name = unit.name;
        g.output_dir = layout.generated_dir(unit.name);
        g.log_file = layout.log_file(unit.name, "generate");
        const std::string key = generate_key(report.generator_id, *rec);
        const fs::path record_file = layout.generate_record(unit.name);
        const std::optional<GenerateRecord> record = read_generate_record(record_file);
        std::error_code ec;
        if (!o.force && record && record->key == key &&
            fs::is_regular_file(g.output_dir / "register_functions.cpp", ec)) {
            g.skipped = true;
            g.result = record->result;
            progress(o, unit.name, "generated sources are up to date");
            report.units.push_back(std::move(g));
            continue;
        }
        progress(o, unit.name, "running ps2_recomp");
        GenerateOptions go;
        go.ps2_recomp = *exe;
        go.config = rec->config;
        go.output_dir = g.output_dir;
        go.log_file = g.log_file;
        go.cancel = o.cancel;
        go.on_output = o.on_tool_output;
        fs::remove(record_file, ec); // a failed run must not leave a matching key behind
        g.result = run_recompiler(go);
        write_generate_record(record_file, GenerateRecord{key, report.generator_id, g.result});
        progress(o, unit.name,
                 std::to_string(g.result.report.count("functions_recompiled")) + " functions recompiled, " +
                     std::to_string(g.result.report.count("unhandled_instructions")) +
                     " unhandled instructions, " + std::to_string(g.result.files) + " files (" +
                     std::to_string(g.result.files_unchanged) + " unchanged)");
        report.units.push_back(std::move(g));
    }
    return report;
}

BuildReport build_units(const VerifiedImage& v, const PipelineOptions& o) {
    const WorkLayout layout(o.work_dir);
    const std::vector<CodeUnit> all = code_units(v);
    const std::vector<CodeUnit> selected = select_units(all, o.units);
    const UnitsFile units = read_units_file(layout, v);

    BuildReport report;
    report.jobs = o.jobs != 0 ? o.jobs : default_build_jobs();
    report.modules_dir = layout.modules_dir(v.profile.id);
    report.manifest = layout.build_manifest(v.profile.id);

    // The manifest keeps the rows of units this run does not rebuild.
    std::map<std::string, toml::ordered_value> rows;
    std::error_code ec;
    if (fs::is_regular_file(report.manifest, ec)) {
        try {
            const toml::ordered_value old = read_toml(report.manifest);
            if (str_of(old, "profile") == v.profile.id && str_of(old, "boot_sha1") == v.boot.sha1 &&
                old.contains("units") && old.at("units").is_array()) {
                for (const toml::ordered_value& t : old.at("units").as_array()) {
                    rows[str_of(t, "name")] = t;
                }
            }
        } catch (const Error&) {
        }
    }

    for (const CodeUnit& unit : selected) {
        check_cancel(o);
        const UnitRecord* rec = units.find(unit.name);
        if (rec == nullptr) {
            throw Error(ErrorCode::work_stale,
                        "unit '" + unit.name + "' has not been prepared; run `dq8ctl recomp prepare` first");
        }
        const std::optional<GenerateRecord> generated =
            read_generate_record(layout.generate_record(unit.name));
        const fs::path generated_dir = layout.generated_dir(unit.name);
        if (!generated || !fs::is_regular_file(generated_dir / "register_functions.cpp", ec)) {
            throw Error(ErrorCode::work_stale,
                        "unit '" + unit.name +
                            "' has no generated sources; run `dq8ctl recomp generate` first");
        }

        BuiltUnit b;
        b.name = unit.name;
        b.log_file = layout.log_file(unit.name, "build");
        fs::remove(b.log_file, ec);
        ModuleUnit mu;
        mu.name = unit.name;
        mu.kind = rec->kind;
        mu.overlay_id = rec->overlay_id;
        mu.profile_id = v.profile.id;
        mu.source_sha1 = rec->source_sha1;
        mu.entry_point = rec->entry;
        mu.generated_dir = generated_dir;
        ModuleBuildOptions mo;
        mo.cmake = o.cmake;
        mo.cmake_generator = o.cmake_generator;
        mo.module_source_dir = o.module_source_dir;
        mo.abi_include_dir = o.abi_include_dir;
        mo.build_dir = layout.build_dir(unit.name);
        mo.generator_id = generated->generator;
        mo.pch = o.pch;
        mo.debug_info = o.debug_info;
        mo.jobs = report.jobs;
        mo.extra_configure_args = o.extra_cmake_args;
        mo.log_file = b.log_file;
        mo.cancel = o.cancel;
        mo.on_output = o.on_tool_output;
        progress(o, unit.name, "building the module with " + std::to_string(report.jobs) + " jobs");
        b.result = build_module(mo, mu);
        b.module = report.modules_dir / b.result.library.filename();
        b.module_updated = copy_if_changed(b.result.library, b.module);

        std::size_t files = 0;
        std::uint64_t bytes = 0;
        const std::string generated_sha1 = tree_sha1(generated_dir, files, bytes);
        toml::ordered_value t(toml::ordered_table{});
        t["name"] = unit.name;
        t["kind"] = std::string(to_string(rec->kind));
        t["overlay_id"] = static_cast<std::int64_t>(rec->overlay_id);
        t["module"] = path_utf8(b.module.filename());
        t["module_sha1"] = platform::sha1_file(b.module);
        t["source_path"] = rec->source_path;
        t["source_sha1"] = rec->source_sha1;
        t["elf_sha1"] = platform::sha1_file(rec->elf);
        t["config_sha1"] = platform::sha1_file(rec->config);
        t["map_sha1"] = platform::sha1_file(rec->map);
        t["generator"] = generated->generator;
        t["generated_files"] = static_cast<std::int64_t>(files);
        t["generated_bytes"] = static_cast<std::int64_t>(bytes);
        t["generated_sha1"] = generated_sha1;
        t["functions_recompiled"] =
            static_cast<std::int64_t>(generated->result.report.count("functions_recompiled"));
        t["unhandled_instructions"] =
            static_cast<std::int64_t>(generated->result.report.count("unhandled_instructions"));
        t["jobs"] = static_cast<std::int64_t>(b.result.jobs);
        t["generate_seconds"] = seconds(generated->result.elapsed);
        t["configure_seconds"] = seconds(b.result.configure_time);
        t["build_seconds"] = seconds(b.result.build_time);
        t["built_at"] = utc_timestamp();
        rows[unit.name] = std::move(t);
        progress(o, unit.name,
                 path_utf8(b.module.filename()) + (b.module_updated ? " updated" : " unchanged") +
                     " (build " + std::to_string(b.result.build_time.count() / 1000) + " s)");
        report.units.push_back(std::move(b));
    }

    toml::ordered_value list(toml::ordered_array{});
    for (const CodeUnit& u : all) {
        if (const auto it = rows.find(u.name); it != rows.end()) {
            list.as_array().push_back(it->second);
        }
    }
    list.as_array_fmt().fmt = toml::array_format::array_of_tables;
    toml::ordered_value root(toml::ordered_table{});
    root["schema_version"] = manifest_schema_version;
    root["profile"] = v.profile.id;
    root["boot_sha1"] = v.boot.sha1;
    root["tool_version"] = std::string(version());
    root["units"] = std::move(list);
    platform::write_file_atomic(
        report.manifest,
        header_comment("Game modules built by `dq8ctl recomp build`: input hashes, generator, "
                       "timings.") +
            toml::format(root));
    return report;
}

} // namespace dq8::core::recomp
