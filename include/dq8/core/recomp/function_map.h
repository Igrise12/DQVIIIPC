// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Function maps: the per-unit input that tells the recompiler where functions start and end. Contributors
// export them from Ghidra (tools/ghidra/ExportFunctionMap.java) into one directory, three files per unit:
//
//   <unit>.csv               "Name,Start,End,Size" header, then one row per function; Start and End are
//                            addresses ("0x%08X" or decimal), End is exclusive, Size == End - Start
//   <unit>.jump_tables.toml  [[jump_tables.table]] address, base_register, entries = [{index, target}, ...]
//                            (optional; the table's data address keys the table, as the recompiler expects)
//   <unit>.stats.toml        link_return_entries = [0x...] (return points of bgezal/bltzal/bal calls that
//                            leave their function) plus integer counts (optional)
//
// Readers report malformed input as Error(map_parse) and inconsistent input as Error(map_invalid), both with
// "<file>:<line>: " in front of the message. Maps describe a disc's code (addresses), so they stay local.
namespace dq8::core::recomp {

struct MapFunction {
    std::string name;
    std::uint32_t start = 0;
    std::uint32_t end = 0; // exclusive
    std::size_t line = 0;  // 1-based line in the CSV, for messages
};

struct JumpTableEntry {
    std::uint32_t index = 0;
    std::uint32_t target = 0;
};

struct JumpTable {
    std::uint32_t address = 0;       // where the table's words live (data address)
    std::uint32_t base_register = 0; // the GPR the indirect jump goes through
    std::vector<JumpTableEntry> entries;
    std::size_t line = 0;
};

struct FunctionMap {
    std::string unit;
    std::vector<MapFunction> functions;                      // sorted by start
    std::vector<JumpTable> jump_tables;                      // sorted by address
    std::vector<std::uint32_t> link_return_entries;          // sorted, unique
    std::vector<std::pair<std::string, std::int64_t>> stats; // the stats file's other integer keys, in order
    std::filesystem::path csv_path;                          // the files read; empty when absent
    std::filesystem::path jump_tables_path;
    std::filesystem::path stats_path;
};

// Parses a CSV map. `source` names the file in messages. The first line must be the header (the recompiler
// skips line 1 unconditionally, so a missing header would silently drop a function). Rows must have exactly
// four fields, a non-empty printable name without surrounding spaces, 4-aligned Start < End and Size equal to
// End - Start. Rows come back in file order; validate_function_map() sorts and checks ranges.
std::vector<MapFunction> parse_function_csv(std::string_view text, const std::string& source);

// Parses a jump-table fragment ([[jump_tables.table]]). Addresses, indices and targets may be TOML integers
// or numeric strings, as the recompiler accepts; an entry without `index` takes its position.
std::vector<JumpTable> parse_jump_tables(std::string_view text, const std::string& source);

// Parses a stats file into `map.link_return_entries` and `map.stats`.
void parse_map_stats(std::string_view text, const std::string& source, FunctionMap& map);

// Reads <dir>/<unit>.csv (required; Error io_open when missing) and the two optional TOML files.
FunctionMap load_function_map(const std::filesystem::path& dir, std::string_view unit);

// Checks the map against the unit's code range [code_start, code_end) and sorts it. Hard errors
// (Error(map_invalid)): a function outside the range, two functions with the same start, overlapping
// functions, a jump table at address 0 or misaligned, a base register above 31, a table without entries, a
// target outside the range or misaligned, a repeated index, a misaligned or out-of-range link-return entry.
// Soft findings (a link-return entry inside no mapped function, which the recompiler would ignore) are
// returned as warnings.
std::vector<std::string> validate_function_map(FunctionMap& map, std::uint32_t code_start,
                                               std::uint32_t code_end);

} // namespace dq8::core::recomp
