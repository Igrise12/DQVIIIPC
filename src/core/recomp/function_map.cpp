// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/recomp/function_map.h"

#include "dq8/core/elf_headers.h"
#include "dq8/core/error.h"
#include "dq8/core/recomp/recomp_config.h"
#include "platform/files.h"
#include "recomp/common.h"

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <toml.hpp>

namespace dq8::core::recomp {

namespace fs = std::filesystem;
using detail::hex8;
using detail::parse_u32;

namespace {

[[noreturn]] void fail(ErrorCode code, const std::string& source, std::size_t line,
                       const std::string& message) {
    throw Error(code, source + (line != 0 ? ":" + std::to_string(line) : std::string()) + ": " + message);
}

std::string trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t");
    return std::string(s.substr(first, last - first + 1));
}

std::vector<std::string_view> split(std::string_view line, char sep) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = line.find(sep, start);
        out.push_back(
            line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) {
            return out;
        }
        start = end + 1;
    }
}

toml::ordered_value parse_toml(std::string_view text, const std::string& source) {
    std::istringstream stream{std::string(text)};
    auto parsed = toml::try_parse<toml::ordered_type_config>(stream, source);
    if (parsed.is_err()) {
        std::string message;
        for (const toml::error_info& e : parsed.unwrap_err()) {
            const std::size_t line =
                e.locations().empty() ? 0 : e.locations().front().first.first_line_number();
            message += (message.empty() ? "" : "\n") + source + ":" + std::to_string(line) + ": " +
                       e.title() + "\n" + toml::format_error(e);
        }
        throw Error(ErrorCode::map_parse, message);
    }
    return std::move(parsed.unwrap());
}

std::size_t line_of(const toml::ordered_value& v) {
    return v.location().first_line_number();
}

// An address-like TOML value: an integer in [0, 2^32) or a numeric string, as the recompiler accepts.
std::uint32_t u32_of(const toml::ordered_value& v, const std::string& source, const std::string& what) {
    if (v.is_integer()) {
        const std::int64_t n = v.as_integer();
        if (n < 0 || n > 0xFFFFFFFFLL) {
            fail(ErrorCode::map_parse, source, line_of(v), what + " is out of range");
        }
        return static_cast<std::uint32_t>(n);
    }
    if (v.is_string()) {
        if (const auto n = parse_u32(v.as_string())) {
            return *n;
        }
    }
    fail(ErrorCode::map_parse, source, line_of(v), what + " must be an integer or a numeric string");
}

std::string range_text(std::uint32_t start, std::uint32_t end) {
    return "[" + hex8(start) + ", " + hex8(end) + ")";
}

// --- published maps ----------------------------------------------------------------------------------------

bool all_of_chars(std::string_view s, bool (*ok)(char)) {
    return !s.empty() && std::all_of(s.begin(), s.end(), ok);
}

bool is_lower_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool is_profile_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool is_unit_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

bool is_identifier(std::string_view s) {
    if (s.empty() || (s.front() >= '0' && s.front() <= '9')) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

// A published integer: a TOML integer in [0, 2^32). Strings are refused (the format has no numeric strings).
std::uint32_t published_u32(const toml::ordered_value& v, const std::string& source,
                            const std::string& what) {
    if (!v.is_integer()) {
        fail(ErrorCode::map_parse, source, line_of(v), what + " must be an integer");
    }
    const std::int64_t n = v.as_integer();
    if (n < 0 || n > 0xFFFFFFFFLL) {
        fail(ErrorCode::map_parse, source, line_of(v), what + " is out of range");
    }
    return static_cast<std::uint32_t>(n);
}

const std::string& published_string(const toml::ordered_value& root, const char* key,
                                    const std::string& source) {
    if (!root.contains(key)) {
        fail(ErrorCode::map_parse, source, 0, std::string("the map has no '") + key + "'");
    }
    const toml::ordered_value& v = root.at(key);
    if (!v.is_string()) {
        fail(ErrorCode::map_parse, source, line_of(v), std::string(key) + " must be a string");
    }
    return v.as_string();
}

// `what` is a C string so that GCC's -Wdangling-reference sees no temporary in the calls.
const toml::ordered_array& published_array(const toml::ordered_value& v, const std::string& source,
                                           const char* what) {
    if (!v.is_array()) {
        fail(ErrorCode::map_parse, source, line_of(v), std::string(what) + " must be an array");
    }
    return v.as_array();
}

// Each table of an array of tables, with exactly the keys `keys`.
void published_row(const toml::ordered_value& v, const std::string& source, const std::string& what,
                   std::initializer_list<const char*> keys) {
    if (!v.is_table()) {
        fail(ErrorCode::map_parse, source, line_of(v), "each " + what + " must be a table");
    }
    for (const auto& [key, value] : v.as_table()) {
        if (std::none_of(keys.begin(), keys.end(), [&](const char* k) { return key == k; })) {
            fail(ErrorCode::map_parse, source, line_of(value), what + " has an unknown key '" + key + "'");
        }
    }
    for (const char* k : keys) {
        if (!v.contains(k)) {
            fail(ErrorCode::map_parse, source, line_of(v), what + " needs '" + k + "'");
        }
    }
}

} // namespace

std::vector<MapFunction> parse_function_csv(std::string_view text, const std::string& source) {
    std::vector<MapFunction> rows;
    std::size_t line_no = 0;
    bool header_seen = false;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++line_no;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!header_seen) {
            std::string header = trim(line);
            std::transform(header.begin(), header.end(), header.begin(), [](char c) {
                return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            });
            if (header != "name,start,end,size") {
                fail(ErrorCode::map_parse, source, line_no,
                     "the first line must be the header 'Name,Start,End,Size' (the recompiler skips line 1)");
            }
            header_seen = true;
            continue;
        }
        if (trim(line).empty()) {
            continue;
        }
        const std::vector<std::string_view> fields = split(line, ',');
        if (fields.size() != 4) {
            fail(ErrorCode::map_parse, source, line_no,
                 "expected 4 fields (Name,Start,End,Size), found " + std::to_string(fields.size()));
        }
        MapFunction f;
        f.name = std::string(fields[0]);
        f.line = line_no;
        if (f.name.empty() || f.name != trim(f.name)) {
            fail(ErrorCode::map_parse, source, line_no, "the name is empty or has surrounding spaces");
        }
        if (!std::all_of(f.name.begin(), f.name.end(),
                         [](char c) { return c >= 0x20 && c <= 0x7E && c != '"'; })) {
            fail(ErrorCode::map_parse, source, line_no, "the name '" + f.name + "' is not printable ASCII");
        }
        const auto start = parse_u32(trim(fields[1]));
        const auto finish = parse_u32(trim(fields[2]));
        const auto size = parse_u32(trim(fields[3]));
        if (!start || !finish || !size) {
            fail(ErrorCode::map_parse, source, line_no,
                 "Start, End and Size must be 0x-prefixed hex or decimal numbers below 2^32");
        }
        f.start = *start;
        f.end = *finish;
        if (f.start >= f.end) {
            fail(ErrorCode::map_parse, source, line_no,
                 f.name + ": Start " + hex8(f.start) + " is not below End " + hex8(f.end) +
                     " (End is exclusive)");
        }
        if (*size != f.end - f.start) {
            fail(ErrorCode::map_parse, source, line_no,
                 f.name + ": Size " + std::to_string(*size) + " is not End - Start (" +
                     std::to_string(f.end - f.start) + ")");
        }
        if (f.start % 4 != 0 || f.end % 4 != 0) {
            fail(ErrorCode::map_parse, source, line_no, f.name + ": Start and End must be multiples of 4");
        }
        rows.push_back(std::move(f));
    }
    if (!header_seen) {
        fail(ErrorCode::map_parse, source, 1, "the map is empty (no 'Name,Start,End,Size' header)");
    }
    return rows;
}

std::vector<JumpTable> parse_jump_tables(std::string_view text, const std::string& source) {
    const toml::ordered_value root = parse_toml(text, source);
    std::vector<JumpTable> tables;
    if (!root.contains("jump_tables")) {
        return tables;
    }
    const toml::ordered_value& section = root.at("jump_tables");
    if (!section.is_table()) {
        fail(ErrorCode::map_parse, source, line_of(section), "jump_tables must be a table");
    }
    if (!section.contains("table")) {
        return tables;
    }
    const toml::ordered_value& list = section.at("table");
    if (!list.is_array()) {
        fail(ErrorCode::map_parse, source, line_of(list), "jump_tables.table must be an array of tables");
    }
    for (const toml::ordered_value& t : list.as_array()) {
        if (!t.is_table()) {
            fail(ErrorCode::map_parse, source, line_of(t), "each [[jump_tables.table]] must be a table");
        }
        JumpTable table;
        table.line = line_of(t);
        if (!t.contains("address") || !t.contains("entries")) {
            fail(ErrorCode::map_parse, source, table.line, "a jump table needs 'address' and 'entries'");
        }
        table.address = u32_of(t.at("address"), source, "address");
        table.base_register =
            t.contains("base_register") ? u32_of(t.at("base_register"), source, "base_register") : 0;
        const toml::ordered_value& entries = t.at("entries");
        if (!entries.is_array()) {
            fail(ErrorCode::map_parse, source, line_of(entries),
                 "entries must be an array of {index, target}");
        }
        std::uint32_t position = 0;
        for (const toml::ordered_value& e : entries.as_array()) {
            if (!e.is_table() || !e.contains("target")) {
                fail(ErrorCode::map_parse, source, line_of(e), "each entry must be a table with a 'target'");
            }
            JumpTableEntry entry;
            entry.index = e.contains("index") ? u32_of(e.at("index"), source, "index") : position;
            entry.target = u32_of(e.at("target"), source, "target");
            table.entries.push_back(entry);
            ++position;
        }
        tables.push_back(std::move(table));
    }
    return tables;
}

void parse_map_stats(std::string_view text, const std::string& source, FunctionMap& map) {
    const toml::ordered_value root = parse_toml(text, source);
    for (const auto& [key, value] : root.as_table()) {
        if (key == "link_return_entries") {
            if (!value.is_array()) {
                fail(ErrorCode::map_parse, source, line_of(value), "link_return_entries must be an array");
            }
            for (const toml::ordered_value& v : value.as_array()) {
                map.link_return_entries.push_back(u32_of(v, source, "a link_return_entries element"));
            }
        } else if (value.is_integer()) {
            map.stats.emplace_back(key, value.as_integer());
        } else if (key == "counts" && value.is_table()) {
            for (const auto& [count_key, count] : value.as_table()) {
                if (count.is_integer()) {
                    map.stats.emplace_back(count_key, count.as_integer());
                }
            }
        }
    }
}

FunctionMap load_function_map(const fs::path& dir, std::string_view unit) {
    FunctionMap map;
    map.unit = std::string(unit);
    const fs::path csv = dir / (std::string(unit) + ".csv");
    std::error_code ec;
    if (!fs::is_regular_file(csv, ec)) {
        throw Error(ErrorCode::io_open, "no function map for unit '" + map.unit + "': '" +
                                            detail::path_utf8(csv) +
                                            "' does not exist (tools/ghidra/export_maps.sh writes the maps)");
    }
    map.csv_path = csv;
    map.functions = parse_function_csv(platform::read_text_file(csv), detail::path_utf8(csv));
    const fs::path tables = dir / (std::string(unit) + ".jump_tables.toml");
    if (fs::is_regular_file(tables, ec)) {
        map.jump_tables_path = tables;
        map.jump_tables = parse_jump_tables(platform::read_text_file(tables), detail::path_utf8(tables));
    }
    const fs::path stats = dir / (std::string(unit) + ".stats.toml");
    if (fs::is_regular_file(stats, ec)) {
        map.stats_path = stats;
        parse_map_stats(platform::read_text_file(stats), detail::path_utf8(stats), map);
    }
    return map;
}

std::vector<std::string> validate_function_map(FunctionMap& map, std::uint32_t code_start,
                                               std::uint32_t code_end) {
    const std::string csv = map.csv_path.empty() ? map.unit + ".csv" : detail::path_utf8(map.csv_path);
    const std::string tables = map.jump_tables_path.empty() ? map.unit + ".jump_tables.toml"
                                                            : detail::path_utf8(map.jump_tables_path);
    const std::string stats =
        map.stats_path.empty() ? map.unit + ".stats.toml" : detail::path_utf8(map.stats_path);
    const std::string unit_range = "the unit's code range " + range_text(code_start, code_end);
    auto inside = [&](std::uint32_t a) { return a >= code_start && a < code_end; };

    std::stable_sort(map.functions.begin(), map.functions.end(),
                     [](const MapFunction& a, const MapFunction& b) { return a.start < b.start; });
    for (std::size_t i = 0; i < map.functions.size(); ++i) {
        const MapFunction& f = map.functions[i];
        if (!inside(f.start) || f.end > code_end) {
            fail(ErrorCode::map_invalid, csv, f.line,
                 f.name + " " + range_text(f.start, f.end) + " lies outside " + unit_range);
        }
        if (i > 0) {
            const MapFunction& prev = map.functions[i - 1];
            if (prev.start == f.start) {
                fail(ErrorCode::map_invalid, csv, f.line,
                     f.name + " starts at " + hex8(f.start) + " like " + prev.name + " (line " +
                         std::to_string(prev.line) + ")");
            }
            if (prev.end > f.start) {
                fail(ErrorCode::map_invalid, csv, f.line,
                     f.name + " " + range_text(f.start, f.end) + " overlaps " + prev.name + " " +
                         range_text(prev.start, prev.end) + " (line " + std::to_string(prev.line) + ")");
            }
        }
    }

    std::sort(map.jump_tables.begin(), map.jump_tables.end(),
              [](const JumpTable& a, const JumpTable& b) { return a.address < b.address; });
    for (std::size_t i = 0; i < map.jump_tables.size(); ++i) {
        const JumpTable& t = map.jump_tables[i];
        const std::string label = "the jump table at " + hex8(t.address);
        if (t.address == 0 || t.address % 4 != 0) {
            fail(ErrorCode::map_invalid, tables, t.line, label + " has a zero or misaligned address");
        }
        if (i > 0 && map.jump_tables[i - 1].address == t.address) {
            fail(ErrorCode::map_invalid, tables, t.line, label + " appears twice");
        }
        if (t.base_register > 31) {
            fail(ErrorCode::map_invalid, tables, t.line,
                 label + " names base_register " + std::to_string(t.base_register) + " (0..31)");
        }
        if (t.entries.empty()) {
            fail(ErrorCode::map_invalid, tables, t.line, label + " has no entries");
        }
        std::set<std::uint32_t> indices;
        for (const JumpTableEntry& e : t.entries) {
            if (!indices.insert(e.index).second) {
                fail(ErrorCode::map_invalid, tables, t.line,
                     label + " repeats index " + std::to_string(e.index));
            }
            if (!inside(e.target) || e.target % 4 != 0) {
                fail(ErrorCode::map_invalid, tables, t.line,
                     label + ": target " + hex8(e.target) + " (index " + std::to_string(e.index) +
                         ") is misaligned or outside " + unit_range);
            }
        }
    }

    std::vector<std::string> warnings;
    std::sort(map.link_return_entries.begin(), map.link_return_entries.end());
    map.link_return_entries.erase(std::unique(map.link_return_entries.begin(), map.link_return_entries.end()),
                                  map.link_return_entries.end());
    for (const std::uint32_t a : map.link_return_entries) {
        if (!inside(a) || a % 4 != 0) {
            fail(ErrorCode::map_invalid, stats, 0,
                 "link-return entry " + hex8(a) + " is misaligned or outside " + unit_range);
        }
        const auto owner =
            std::upper_bound(map.functions.begin(), map.functions.end(), a,
                             [](std::uint32_t v, const MapFunction& f) { return v < f.start; });
        if (owner == map.functions.begin() || std::prev(owner)->end <= a) {
            warnings.push_back("link-return entry " + hex8(a) +
                               " lies in no mapped function; the recompiler will ignore it");
        }
    }
    return warnings;
}

PublishedMap parse_published_map(std::string_view text, const std::string& source) {
    const toml::ordered_value root = parse_toml(text, source);
    static constexpr const char* keys[] = {
        "format", "profile",   "unit", "source_sha1", "code", "functions", "link_return_entries",
        "named",  "jump_table"};
    for (const auto& [key, value] : root.as_table()) {
        if (std::none_of(std::begin(keys), std::end(keys), [&](const char* k) { return key == k; })) {
            fail(ErrorCode::map_parse, source, line_of(value),
                 "unknown key '" + key + "' (Docs/formats/function-map.md lists the published keys)");
        }
    }
    if (!root.contains("format") || !root.at("format").is_integer()) {
        fail(ErrorCode::map_parse, source, 0, "the map needs an integer 'format'");
    }
    if (root.at("format").as_integer() != published_map_format) {
        fail(ErrorCode::map_parse, source, line_of(root.at("format")),
             "format " + std::to_string(root.at("format").as_integer()) +
                 " is not known (this program reads " + std::to_string(published_map_format) + ")");
    }

    PublishedMap map;
    map.profile = published_string(root, "profile", source);
    map.unit = published_string(root, "unit", source);
    map.source_sha1 = published_string(root, "source_sha1", source);
    if (!all_of_chars(map.profile, is_profile_char)) {
        fail(ErrorCode::map_parse, source, line_of(root.at("profile")), "profile must be [A-Z0-9_]+");
    }
    if (!all_of_chars(map.unit, is_unit_char)) {
        fail(ErrorCode::map_parse, source, line_of(root.at("unit")), "unit must be [a-z0-9_]+");
    }
    if (map.source_sha1.size() != 40 || !all_of_chars(map.source_sha1, is_lower_hex)) {
        fail(ErrorCode::map_parse, source, line_of(root.at("source_sha1")),
             "source_sha1 must be 40 lower-case hex digits");
    }

    if (!root.contains("code")) {
        fail(ErrorCode::map_parse, source, 0, "the map has no 'code'");
    }
    const toml::ordered_array& code = published_array(root.at("code"), source, "code");
    if (code.size() != 2) {
        fail(ErrorCode::map_parse, source, line_of(root.at("code")), "code must be [start, end]");
    }
    map.code_start = published_u32(code[0], source, "code's start");
    map.code_end = published_u32(code[1], source, "code's end");
    const std::size_t code_line = line_of(root.at("code"));
    if (map.code_start >= map.code_end || map.code_start % 4 != 0 || map.code_end % 4 != 0) {
        fail(ErrorCode::map_invalid, source, code_line,
             "code " + range_text(map.code_start, map.code_end) + " must be a non-empty 4-aligned range");
    }
    const std::string unit_range = "the code range " + range_text(map.code_start, map.code_end);
    auto inside = [&](std::uint32_t a) { return a >= map.code_start && a < map.code_end; };

    if (!root.contains("functions")) {
        fail(ErrorCode::map_parse, source, 0, "the map has no 'functions'");
    }
    for (const toml::ordered_value& v : published_array(root.at("functions"), source, "functions")) {
        if (!v.is_array() || v.as_array().size() != 2) {
            fail(ErrorCode::map_parse, source, line_of(v), "each function must be [start, size]");
        }
        PublishedFunction f;
        f.start = published_u32(v.as_array()[0], source, "a function's start");
        f.size = published_u32(v.as_array()[1], source, "a function's size");
        const std::uint64_t end = std::uint64_t{f.start} + f.size;
        const std::string label = "the function at " + hex8(f.start);
        if (f.size == 0 || f.start % 4 != 0 || f.size % 4 != 0) {
            fail(ErrorCode::map_invalid, source, line_of(v), label + " is empty or misaligned");
        }
        if (!inside(f.start) || end > map.code_end) {
            fail(ErrorCode::map_invalid, source, line_of(v), label + " lies outside " + unit_range);
        }
        if (!map.functions.empty()) {
            const PublishedFunction& prev = map.functions.back();
            if (prev.start >= f.start) {
                fail(ErrorCode::map_invalid, source, line_of(v), label + " is out of order or repeated");
            }
            if (std::uint64_t{prev.start} + prev.size > f.start) {
                fail(ErrorCode::map_invalid, source, line_of(v),
                     label + " overlaps the function at " + hex8(prev.start));
            }
        }
        map.functions.push_back(f);
    }

    if (root.contains("link_return_entries")) {
        for (const toml::ordered_value& v :
             published_array(root.at("link_return_entries"), source, "link_return_entries")) {
            const std::uint32_t a = published_u32(v, source, "a link-return entry");
            if (!inside(a) || a % 4 != 0) {
                fail(ErrorCode::map_invalid, source, line_of(v),
                     "link-return entry " + hex8(a) + " is misaligned or outside " + unit_range);
            }
            if (!map.link_return_entries.empty() && map.link_return_entries.back() >= a) {
                fail(ErrorCode::map_invalid, source, line_of(v),
                     "link-return entry " + hex8(a) + " is out of order or repeated");
            }
            map.link_return_entries.push_back(a);
        }
    }

    if (root.contains("named")) {
        for (const toml::ordered_value& v : published_array(root.at("named"), source, "named")) {
            published_row(v, source, "[[named]]", {"start", "name"});
            PublishedName n;
            n.line = line_of(v);
            n.start = published_u32(v.at("start"), source, "[[named]] start");
            if (!v.at("name").is_string()) {
                fail(ErrorCode::map_parse, source, n.line, "[[named]] name must be a string");
            }
            n.name = v.at("name").as_string();
            const auto f = std::lower_bound(
                map.functions.begin(), map.functions.end(), n.start,
                [](const PublishedFunction& fn, std::uint32_t start) { return fn.start < start; });
            if (f == map.functions.end() || f->start != n.start) {
                fail(ErrorCode::map_invalid, source, n.line, hex8(n.start) + " is no function's start");
            }
            if (!is_identifier(n.name) || resolve_runtime_call(n.name).kind == RuntimeCallKind::none) {
                fail(ErrorCode::map_invalid, source, n.line,
                     "'" + n.name + "' is not a kernel or SDK name of the runtime's call list");
            }
            if (!map.named.empty() && map.named.back().start >= n.start) {
                fail(ErrorCode::map_invalid, source, n.line,
                     "[[named]] at " + hex8(n.start) + " is out of order or repeated");
            }
            map.named.push_back(std::move(n));
        }
    }

    if (root.contains("jump_table")) {
        for (const toml::ordered_value& v : published_array(root.at("jump_table"), source, "jump_table")) {
            published_row(v, source, "[[jump_table]]", {"address", "base_register", "count"});
            PublishedJumpTable t;
            t.line = line_of(v);
            t.address = published_u32(v.at("address"), source, "[[jump_table]] address");
            t.base_register = published_u32(v.at("base_register"), source, "[[jump_table]] base_register");
            t.count = published_u32(v.at("count"), source, "[[jump_table]] count");
            const std::string label = "the jump table at " + hex8(t.address);
            if (t.address == 0 || t.address % 4 != 0) {
                fail(ErrorCode::map_invalid, source, t.line, label + " has a zero or misaligned address");
            }
            if (t.base_register > 31) {
                fail(ErrorCode::map_invalid, source, t.line,
                     label + " names base_register " + std::to_string(t.base_register) + " (0..31)");
            }
            if (t.count == 0 || std::uint64_t{t.address} + 4ull * t.count > 0x100000000ull) {
                fail(ErrorCode::map_invalid, source, t.line,
                     label + " has count " + std::to_string(t.count) + " (at least 1, inside 4 GiB)");
            }
            if (!map.jump_tables.empty() && map.jump_tables.back().address >= t.address) {
                fail(ErrorCode::map_invalid, source, t.line, label + " is out of order or repeated");
            }
            map.jump_tables.push_back(t);
        }
    }
    return map;
}

fs::path published_map_file(const fs::path& root, std::string_view profile, std::string_view unit) {
    return root / std::string(profile) / (std::string(unit) + ".toml");
}

PublishedMap load_published_map(const fs::path& root, std::string_view profile, std::string_view unit) {
    const fs::path file = published_map_file(root, profile, unit);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        throw Error(ErrorCode::maps_missing,
                    "no published function map for unit '" + std::string(unit) + "' of " +
                        std::string(profile) + ": '" + detail::path_utf8(file) +
                        "' does not exist (contributors: --maps DIR for Ghidra maps)");
    }
    const std::string source = detail::path_utf8(file);
    PublishedMap map = parse_published_map(platform::read_text_file(file), source);
    if (map.profile != profile || map.unit != unit) {
        fail(ErrorCode::map_invalid, source, 0,
             "the map is for " + map.profile + "/" + map.unit + ", not " + std::string(profile) + "/" +
                 std::string(unit));
    }
    map.path = file;
    return map;
}

void check_published_map_source(const PublishedMap& map, std::string_view source_sha1) {
    std::string actual(source_sha1);
    std::transform(actual.begin(), actual.end(), actual.begin(),
                   [](char c) { return static_cast<char>(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c); });
    if (actual != map.source_sha1) {
        throw Error(ErrorCode::maps_missing,
                    "the published function map of unit '" + map.unit + "' (" +
                        (map.path.empty() ? map.profile + "/" + map.unit : detail::path_utf8(map.path)) +
                        ") describes the file with SHA-1 " + map.source_sha1 + ", not this disc's " + actual +
                        " (contributors: --maps DIR for Ghidra maps of this disc)");
    }
}

std::string generated_function_name(std::uint32_t start) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "FUN_%08x", static_cast<unsigned>(start));
    return buf;
}

FunctionMap make_function_map(const PublishedMap& map, std::span<const std::uint8_t> unit_elf) {
    const std::string source =
        map.path.empty() ? map.profile + "/" + map.unit + ".toml" : detail::path_utf8(map.path);
    FunctionMap out;
    out.unit = map.unit;
    std::map<std::uint32_t, const std::string*> names;
    for (const PublishedName& n : map.named) {
        names[n.start] = &n.name;
    }
    out.functions.reserve(map.functions.size());
    for (const PublishedFunction& f : map.functions) {
        MapFunction m;
        const auto it = names.find(f.start);
        m.name = it != names.end() ? *it->second : generated_function_name(f.start);
        m.start = f.start;
        m.end = f.start + f.size;
        out.functions.push_back(std::move(m));
    }
    out.link_return_entries = map.link_return_entries;

    const ElfHeaders headers = parse_elf_headers(unit_elf);
    const std::vector<const ElfSegment*> segments = headers.load_segments();
    // The little-endian word at `address`, from the load segment whose file data holds all four bytes.
    auto word_at = [&](std::uint32_t address) -> std::optional<std::uint32_t> {
        for (const ElfSegment* s : segments) {
            if (address >= s->vaddr && std::uint64_t{address} + 4 <= std::uint64_t{s->vaddr} + s->filesz) {
                const std::uint64_t offset = std::uint64_t{s->offset} + (address - s->vaddr);
                if (offset + 4 > unit_elf.size()) {
                    return std::nullopt;
                }
                const std::uint8_t* p = unit_elf.data() + offset;
                return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                       (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
            }
        }
        return std::nullopt;
    };
    for (const PublishedJumpTable& t : map.jump_tables) {
        JumpTable table;
        table.address = t.address;
        table.base_register = t.base_register;
        table.line = t.line;
        // A damaged count fails at its first word outside the image, not in this reservation.
        table.entries.reserve(std::min<std::uint32_t>(t.count, 4096));
        for (std::uint32_t i = 0; i < t.count; ++i) {
            const std::uint32_t address = t.address + 4 * i;
            const std::optional<std::uint32_t> target = word_at(address);
            if (!target) {
                fail(ErrorCode::map_invalid, source, t.line,
                     "the jump table at " + hex8(t.address) + ": entry " + std::to_string(i) + " at " +
                         hex8(address) + " lies outside the unit image's loaded data");
            }
            table.entries.push_back(JumpTableEntry{i, *target});
        }
        out.jump_tables.push_back(std::move(table));
    }
    return out;
}

std::string function_csv_text(const FunctionMap& map) {
    std::vector<const MapFunction*> functions;
    functions.reserve(map.functions.size());
    for (const MapFunction& f : map.functions) {
        functions.push_back(&f);
    }
    std::stable_sort(functions.begin(), functions.end(),
                     [](const MapFunction* a, const MapFunction* b) { return a->start < b->start; });
    std::string out = "Name,Start,End,Size\n";
    out.reserve(out.size() + functions.size() * 48);
    for (const MapFunction* f : functions) {
        out += f->name + "," + hex8(f->start) + "," + hex8(f->end) + "," + std::to_string(f->end - f->start) +
               "\n";
    }
    return out;
}

fs::path find_maps_dir(const fs::path& executable_dir) {
    std::vector<fs::path> candidates{fs::path("data") / "maps"};
    fs::path dir = executable_dir;
    for (int up = 0; up < 6 && !dir.empty(); ++up) {
        candidates.push_back(dir / "data" / "maps");
        candidates.push_back(dir / "share" / "dq8" / "maps");
        if (dir == dir.parent_path()) {
            break;
        }
        dir = dir.parent_path();
    }
    std::error_code ec;
    for (const fs::path& c : candidates) {
        if (fs::is_directory(c, ec)) {
            return c;
        }
    }
    throw Error(ErrorCode::maps_missing,
                "no published function maps found (data/maps in the source tree, "
                "share/dq8/maps in an install); pass --maps DIR for contributor maps");
}

} // namespace dq8::core::recomp
