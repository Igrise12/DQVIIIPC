// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/recomp/pipeline.h"

#include "dq8/core/elf_headers.h"
#include "dq8/core/error.h"
#include "dq8/core/hash.h"
#include "dq8/core/intake.h"
#include "dq8/core/iso9660.h"
#include "dq8/core/mwo3.h"
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
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <toml.hpp>

namespace dq8::core::recomp {

namespace fs = std::filesystem;
using detail::hex8;
using detail::path_utf8;

namespace {

constexpr int record_schema_version = 1; // units.toml and build-manifest.toml
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

std::optional<toml::ordered_value> read_toml(const fs::path& file) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        return std::nullopt;
    }
    try {
        std::istringstream stream(platform::read_text_file(file));
        auto parsed = toml::try_parse<toml::ordered_type_config>(stream, path_utf8(file));
        if (parsed.is_err()) {
            return std::nullopt;
        }
        return std::move(parsed.unwrap());
    } catch (const std::exception&) {
        return std::nullopt; // a damaged record only means the work it described is redone
    }
}

std::string str_of(const toml::ordered_value& t, const char* key) {
    return t.contains(key) && t.at(key).is_string() ? t.at(key).as_string() : std::string();
}

std::int64_t int_of(const toml::ordered_value& t, const char* key) {
    return t.contains(key) && t.at(key).is_integer() ? t.at(key).as_integer() : 0;
}

// True when two build-manifest rows describe the same module from the same inputs (timings aside).
bool same_build(const toml::ordered_value& a, const toml::ordered_value& b) {
    for (const char* key : {"module", "module_sha1", "source_sha1", "elf_sha1", "config_sha1", "map_sha1",
                            "generator", "generated_sha1"}) {
        if (str_of(a, key).empty() || str_of(a, key) != str_of(b, key)) {
            return false;
        }
    }
    return true;
}

// The [[units]] rows of an earlier record written for the same source, by unit name.
std::map<std::string, toml::ordered_value> earlier_rows(const fs::path& file, const std::string& profile,
                                                        const std::string& identity) {
    std::map<std::string, toml::ordered_value> rows;
    const std::optional<toml::ordered_value> old = read_toml(file);
    if (old && str_of(*old, "profile") == profile && str_of(*old, "source") == identity &&
        old->contains("units") && old->at("units").is_array()) {
        for (const toml::ordered_value& t : old->at("units").as_array()) {
            rows[str_of(t, "name")] = t;
        }
    }
    return rows;
}

std::string record_text(const std::string& what, const std::string& profile, const std::string& identity,
                        const std::vector<std::string>& order,
                        std::map<std::string, toml::ordered_value>& rows) {
    toml::ordered_value units(toml::ordered_array{});
    for (const std::string& name : order) {
        if (const auto it = rows.find(name); it != rows.end()) {
            units.as_array().push_back(std::move(it->second));
        }
    }
    units.as_array_fmt().fmt = toml::array_format::array_of_tables;
    toml::ordered_value root(toml::ordered_table{});
    root["schema_version"] = record_schema_version;
    root["profile"] = profile;
    root["source"] = identity;
    root["tool_version"] = std::string(version());
    root["units"] = std::move(units);
    return header_comment(what) + toml::format(root);
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

// --- stamps/<unit>.generate.toml ---------------------------------------------------------------------------

struct GenerateRecord {
    std::string key;
    std::string generator;
    GenerateResult result;
};

std::optional<GenerateRecord> read_generate_record(const fs::path& file) {
    const std::optional<toml::ordered_value> root = read_toml(file);
    if (!root) {
        return std::nullopt;
    }
    GenerateRecord r;
    r.key = str_of(*root, "key");
    r.generator = str_of(*root, "generator");
    r.result.elapsed = std::chrono::milliseconds(int_of(*root, "elapsed_ms"));
    r.result.files = static_cast<std::size_t>(int_of(*root, "files"));
    r.result.bytes = static_cast<std::uint64_t>(int_of(*root, "bytes"));
    r.result.report.found = true;
    if (root->contains("counts") && root->at("counts").is_table()) {
        for (const auto& [k, v] : root->at("counts").as_table()) {
            if (v.is_integer()) {
                r.result.report.counts.emplace_back(k, static_cast<std::uint64_t>(v.as_integer()));
            }
        }
    }
    return r;
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

std::string generate_key(const std::string& generator, const PreparedUnit& u) {
    return sha1_text("generator=" + generator + "\nconfig=" + platform::sha1_file(u.config) + "\nelf=" +
                     platform::sha1_file(u.elf) + "\nmap=" + platform::sha1_file(u.map_csv) + "\n");
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

std::vector<std::string> names_of(const std::vector<PreparedUnit>& units) {
    std::vector<std::string> out;
    for (const PreparedUnit& p : units) {
        out.push_back(p.unit.name);
    }
    return out;
}

} // namespace

UnitSource disc_units(const VerifiedImage& v) {
    UnitSource source;
    source.profile_id = v.profile.id;
    source.identity = v.boot.sha1;
    source.units = code_units(v);
    auto image = std::make_shared<iso::Image>(iso::Image::open(v.image_path));
    const Mwo3Window window{v.profile.overlay_window.base, v.profile.overlay_window.end};
    const std::uint32_t elf_flags = v.elf.flags;
    const std::string boot_sha1 = v.boot.sha1;
    source.load = [image, window, elf_flags, boot_sha1](const CodeUnit& unit) {
        const std::vector<std::uint8_t> bytes =
            image->read_all(image->require(unit.source_path), max_unit_source);
        UnitImage out;
        out.source_sha1 = to_hex(Sha1::of(bytes));
        if (unit.kind == UnitKind::main) {
            if (out.source_sha1 != boot_sha1) {
                throw Error(ErrorCode::image_mismatch, "the boot file read back with SHA-1 " +
                                                           out.source_sha1 + ", not the verified " +
                                                           boot_sha1);
            }
            out.elf = bytes; // the boot ELF is its own unit ELF
        } else {
            out.elf = make_overlay_unit_elf(bytes, elf_flags, window).elf;
        }
        return out;
    };
    return source;
}

UnitSource elf_units(const std::vector<std::pair<std::string, fs::path>>& elfs, std::string profile_id) {
    UnitSource source;
    source.profile_id = std::move(profile_id);
    source.identity = "dev";
    std::set<std::string> seen;
    std::map<std::string, fs::path> paths;
    for (const auto& [name, path] : elfs) {
        const bool valid_name = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        });
        if (!valid_name || !seen.insert(name).second) {
            throw Error(ErrorCode::invalid_argument,
                        "unit name '" + name + "' is repeated or not [a-z0-9_]+");
        }
        const std::vector<std::uint8_t> bytes = platform::read_file(path, max_unit_source);
        const ElfHeaders h = parse_elf_headers(bytes);
        const ElfSegment* text = nullptr;
        for (const ElfSegment* s : h.load_segments()) {
            if ((s->flags & elf_pf_x) != 0 && s->filesz != 0) {
                text = s;
                break;
            }
        }
        if (text == nullptr || static_cast<std::uint64_t>(text->offset) + text->filesz > bytes.size()) {
            throw Error(ErrorCode::elf_format,
                        "'" + path_utf8(path) + "' has no executable load segment with data");
        }
        CodeUnit unit;
        unit.name = name;
        unit.kind = name == "main" ? UnitKind::main : UnitKind::overlay;
        unit.source_path = path_utf8(path);
        unit.entry = unit.kind == UnitKind::main ? h.entry : 0;
        unit.code_start = text->vaddr;
        unit.code_end = text->vaddr + text->filesz;
        unit.load_address = text->vaddr;
        const std::span<const std::uint8_t> segment(bytes.data() + text->offset, text->filesz);
        if (unit.kind == UnitKind::overlay && looks_like_mwo3(segment)) {
            try {
                unit.overlay_id = parse_mwo3_header(segment, segment.size()).overlay_id;
            } catch (const Error&) {
                unit.overlay_id = 0;
            }
        }
        source.units.push_back(unit);
        paths[name] = path;
    }
    // Pipeline order: main first, then the overlays as given.
    std::stable_partition(source.units.begin(), source.units.end(),
                          [](const CodeUnit& u) { return u.kind == UnitKind::main; });
    source.load = [paths](const CodeUnit& unit) {
        UnitImage out;
        out.elf = platform::read_file(paths.at(unit.name), max_unit_source);
        out.source_sha1 = to_hex(Sha1::of(out.elf));
        if (unit.kind == UnitKind::overlay) {
            // The container, when the segment holds one, identifies an overlay as its file does on a disc.
            const ElfHeaders h = parse_elf_headers(out.elf);
            for (const ElfSegment* s : h.load_segments()) {
                if (s->vaddr == unit.load_address && s->filesz != 0) {
                    const std::span<const std::uint8_t> segment(out.elf.data() + s->offset, s->filesz);
                    if (looks_like_mwo3(segment)) {
                        out.source_sha1 = to_hex(Sha1::of(segment));
                    }
                    break;
                }
            }
        }
        return out;
    };
    return source;
}

PrepareReport prepare_units(const UnitSource& source, const PipelineOptions& o) {
    if (o.maps_dir.empty()) {
        throw Error(ErrorCode::invalid_argument, "prepare needs the function-map directory (--maps DIR)");
    }
    const WorkLayout layout(o.work_dir);
    const std::vector<CodeUnit> selected = select_units(source.units, o.units);
    platform::make_directories(layout.root());
    platform::write_file_if_changed(layout.root() / ".gitignore", std::string_view("*\n"));

    PrepareReport report;
    report.profile_id = source.profile_id;
    report.source_identity = source.identity;
    report.work_dir = layout.root();
    report.units_file = layout.units_file();
    std::map<std::string, toml::ordered_value> rows =
        earlier_rows(layout.units_file(), source.profile_id, source.identity);
    for (const CodeUnit& unit : selected) {
        check_cancel(o);
        progress(o, unit.name, "reading " + unit.source_path);
        PreparedUnit p;
        p.unit = unit;
        const UnitImage image = source.load(unit);
        p.source_sha1 = image.source_sha1;
        p.elf = fs::absolute(layout.unit_elf(unit.name));
        p.elf_written = platform::write_file_if_changed(p.elf, image.elf);

        FunctionMap map = load_function_map(o.maps_dir, unit.name);
        p.warnings = validate_function_map(map, unit.code_start, unit.code_end);
        p.map_csv = fs::absolute(map.csv_path);
        p.functions = map.functions.size();
        p.jump_tables = map.jump_tables.size();

        RecompConfigInput in;
        in.unit = unit.name;
        in.profile_id = source.profile_id;
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

        toml::ordered_value t(toml::ordered_table{});
        t["name"] = unit.name;
        t["kind"] = std::string(to_string(unit.kind));
        t["overlay_id"] = static_cast<std::int64_t>(unit.overlay_id);
        t["source_path"] = unit.source_path;
        t["source_sha1"] = p.source_sha1;
        t["entry"] = hex_value(unit.entry);
        t["code_start"] = hex_value(unit.code_start);
        t["code_end"] = hex_value(unit.code_end);
        t["elf"] = path_utf8(p.elf);
        t["config"] = path_utf8(p.config);
        t["map"] = path_utf8(p.map_csv);
        t["functions"] = static_cast<std::int64_t>(p.functions);
        t["stubs"] = static_cast<std::int64_t>(p.stubs);
        t["entry_points"] = static_cast<std::int64_t>(p.entry_points);
        t["jump_tables"] = static_cast<std::int64_t>(p.jump_tables);
        rows[unit.name] = std::move(t);
        report.units.push_back(std::move(p));
    }
    std::vector<std::string> order;
    for (const CodeUnit& u : source.units) {
        order.push_back(u.name);
    }
    platform::write_file_if_changed(layout.units_file(),
                                    record_text("Code units written by `dq8ctl recomp prepare`.",
                                                source.profile_id, source.identity, order, rows));
    return report;
}

GenerateReport generate_units(const PrepareReport& prepared, const PipelineOptions& o) {
    const WorkLayout layout(prepared.work_dir);
    if (o.ps2_recomp.empty()) {
        throw Error(ErrorCode::tool_missing, "generate needs the recompiler (--ps2-recomp PATH)");
    }
    const std::optional<fs::path> exe = find_program(o.ps2_recomp);
    if (!exe) {
        throw Error(ErrorCode::tool_missing,
                    "cannot find the recompiler '" + path_utf8(o.ps2_recomp) + "' (pass --ps2-recomp PATH)");
    }
    GenerateReport report;
    report.ps2_recomp = *exe;
    report.generator_id = "ps2_recomp sha1:" + platform::sha1_file(*exe).substr(0, 12);

    for (const PreparedUnit& unit : prepared.units) {
        check_cancel(o);
        GeneratedUnit g;
        g.name = unit.unit.name;
        g.output_dir = layout.generated_dir(g.name);
        g.log_file = layout.log_file(g.name, "generate");
        const std::string key = generate_key(report.generator_id, unit);
        const fs::path record_file = layout.generate_record(g.name);
        const std::optional<GenerateRecord> record = read_generate_record(record_file);
        std::error_code ec;
        if (!o.force && record && record->key == key &&
            fs::is_regular_file(g.output_dir / "register_functions.cpp", ec)) {
            g.skipped = true;
            g.result = record->result;
            progress(o, g.name, "generated sources are up to date");
            report.units.push_back(std::move(g));
            continue;
        }
        progress(o, g.name, "running ps2_recomp");
        GenerateOptions go;
        go.ps2_recomp = *exe;
        go.config = unit.config;
        go.output_dir = g.output_dir;
        go.log_file = g.log_file;
        go.cancel = o.cancel;
        go.on_output = o.on_tool_output;
        fs::remove(record_file, ec); // a failed run must not leave a matching key behind
        g.result = run_recompiler(go);
        write_generate_record(record_file, GenerateRecord{key, report.generator_id, g.result});
        progress(o, g.name,
                 std::to_string(g.result.report.count("functions_recompiled")) + " functions recompiled, " +
                     std::to_string(g.result.report.count("unhandled_instructions")) +
                     " unhandled instructions, " + std::to_string(g.result.files) + " files (" +
                     std::to_string(g.result.files_unchanged) + " unchanged)");
        report.units.push_back(std::move(g));
    }
    return report;
}

BuildReport build_units(const PrepareReport& prepared, const GenerateReport& generated,
                        const PipelineOptions& o) {
    const WorkLayout layout(prepared.work_dir);
    BuildReport report;
    report.jobs = o.jobs != 0 ? o.jobs : default_build_jobs();
    report.modules_dir = layout.modules_dir(prepared.profile_id);
    report.manifest = layout.build_manifest(prepared.profile_id);

    // The manifest keeps the rows of units this run does not rebuild (when it describes the same source).
    std::map<std::string, toml::ordered_value> rows =
        earlier_rows(report.manifest, prepared.profile_id, prepared.source_identity);
    std::vector<std::string> order;
    if (const std::optional<toml::ordered_value> units = read_toml(prepared.units_file);
        units && units->contains("units") && units->at("units").is_array()) {
        for (const toml::ordered_value& t : units->at("units").as_array()) {
            order.push_back(str_of(t, "name"));
        }
    }
    for (const std::string& name : names_of(prepared.units)) {
        if (std::find(order.begin(), order.end(), name) == order.end()) {
            order.push_back(name);
        }
    }

    for (const PreparedUnit& unit : prepared.units) {
        check_cancel(o);
        const auto gen = std::find_if(generated.units.begin(), generated.units.end(),
                                      [&](const GeneratedUnit& g) { return g.name == unit.unit.name; });
        if (gen == generated.units.end()) {
            throw Error(ErrorCode::work_stale, "unit '" + unit.unit.name + "' was not generated");
        }
        BuiltUnit b;
        b.name = unit.unit.name;
        b.log_file = layout.log_file(b.name, "build");
        std::error_code ec;
        fs::remove(b.log_file, ec);
        ModuleUnit mu;
        mu.name = unit.unit.name;
        mu.kind = unit.unit.kind;
        mu.overlay_id = unit.unit.overlay_id;
        mu.profile_id = prepared.profile_id;
        mu.source_sha1 = unit.source_sha1;
        mu.entry_point = unit.unit.entry;
        mu.generated_dir = fs::absolute(gen->output_dir);
        ModuleBuildOptions mo;
        mo.cmake = o.cmake;
        mo.cmake_generator = o.cmake_generator;
        mo.module_source_dir = o.module_source_dir;
        mo.abi_include_dir = o.abi_include_dir;
        mo.build_dir = layout.build_dir(b.name);
        mo.generator_id = generated.generator_id;
        mo.pch = o.pch;
        mo.debug_info = o.debug_info;
        mo.jobs = report.jobs;
        mo.extra_configure_args = o.extra_cmake_args;
        mo.log_file = b.log_file;
        mo.cancel = o.cancel;
        mo.on_output = o.on_tool_output;
        progress(o, b.name, "building the module with " + std::to_string(report.jobs) + " jobs");
        b.result = build_module(mo, mu);
        b.module = report.modules_dir / b.result.library.filename();
        b.module_updated = copy_if_changed(b.result.library, b.module);

        std::size_t files = 0;
        std::uint64_t bytes = 0;
        const std::string generated_sha1 = tree_sha1(gen->output_dir, files, bytes);
        const RecompReport& rep = gen->result.report;
        toml::ordered_value t(toml::ordered_table{});
        t["name"] = b.name;
        t["kind"] = std::string(to_string(unit.unit.kind));
        t["overlay_id"] = static_cast<std::int64_t>(unit.unit.overlay_id);
        t["module"] = path_utf8(b.module.filename());
        t["module_sha1"] = platform::sha1_file(b.module);
        t["source_path"] = unit.unit.source_path;
        t["source_sha1"] = unit.source_sha1;
        t["elf_sha1"] = platform::sha1_file(unit.elf);
        t["config_sha1"] = platform::sha1_file(unit.config);
        t["map_sha1"] = platform::sha1_file(unit.map_csv);
        t["generator"] = generated.generator_id;
        t["generated_files"] = static_cast<std::int64_t>(files);
        t["generated_bytes"] = static_cast<std::int64_t>(bytes);
        t["generated_sha1"] = generated_sha1;
        t["functions_recompiled"] = static_cast<std::int64_t>(rep.count("functions_recompiled"));
        t["functions_stubbed"] = static_cast<std::int64_t>(rep.count("functions_stubbed"));
        t["unhandled_instructions"] = static_cast<std::int64_t>(rep.count("unhandled_instructions"));
        t["jobs"] = static_cast<std::int64_t>(b.result.jobs);
        t["generate_seconds"] = seconds(gen->result.elapsed);
        t["configure_seconds"] = seconds(b.result.configure_time);
        t["build_seconds"] = seconds(b.result.build_time);
        t["built_at"] = utc_timestamp();
        // A module that came out the same from the same inputs keeps its earlier row and the timings of the
        // build that made it, so a re-run leaves the manifest (and its SHA-1, which the setup's smoke test
        // and the build registry key on) as it was.
        if (const auto earlier = rows.find(b.name);
            b.module_updated || earlier == rows.end() || !same_build(earlier->second, t)) {
            rows[b.name] = std::move(t);
        }
        progress(o, b.name,
                 path_utf8(b.module.filename()) + (b.module_updated ? " updated" : " unchanged") +
                     " (build " + std::to_string(b.result.build_time.count() / 1000) + " s)");
        report.units.push_back(std::move(b));
    }
    platform::write_file_if_changed(
        report.manifest, record_text("Game modules built by `dq8ctl recomp build`: input hashes, generator, "
                                     "timings.",
                                     prepared.profile_id, prepared.source_identity, order, rows));
    return report;
}

} // namespace dq8::core::recomp
