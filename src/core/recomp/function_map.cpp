// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/recomp/function_map.h"

#include "dq8/core/error.h"
#include "platform/files.h"
#include "recomp/common.h"

#include <algorithm>
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

} // namespace dq8::core::recomp
