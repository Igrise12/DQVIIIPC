// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Function maps: the per-unit input that tells the recompiler where functions start and end. They come in two
// forms. Published maps (ADR-0017, Docs/formats/function-map.md) ship with the program under
// data/maps/<profile>/<unit>.toml and hold addresses, sizes and table shapes only; prepare turns one into a
// FunctionMap with make_function_map(), reading the jump tables' entries from the user's unit image.
// Contributor maps are exported from Ghidra (tools/ghidra/ExportFunctionMap.java) into one directory, three
// files per unit:
//
//   <unit>.csv               "Name,Start,End,Size" header, then one row per function; Start and End are
//                            addresses ("0x%08X" or decimal), End is exclusive, Size == End - Start
//   <unit>.jump_tables.toml  [[jump_tables.table]] address, base_register, entries = [{index, target}, ...]
//                            (optional; the table's data address keys the table, as the recompiler expects)
//   <unit>.stats.toml        link_return_entries = [0x...] (return points of bgezal/bltzal/bal calls that
//                            leave their function) plus integer counts, top-level or under [counts]
//                            (optional; other keys are the exporter's diagnostics and are ignored)
//
// Readers report malformed input as Error(map_parse) and inconsistent input as Error(map_invalid), both with
// "<file>:<line>: " in front of the message. Contributor maps carry Ghidra's names and the tables' entries,
// so they stay local; tools/ghidra/publish_maps.py turns them into published maps.
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
    std::vector<std::pair<std::string, std::int64_t>> stats; // integer keys and [counts], in file order
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

// --- Published maps ----------------------------------------------------------------------------------------

inline constexpr std::int64_t published_map_format = 1;

struct PublishedFunction {
    std::uint32_t start = 0;
    std::uint32_t size = 0;
};

struct PublishedName {
    std::uint32_t start = 0;
    std::string name; // resolves against the runtime's call list (resolve_runtime_call)
    std::size_t line = 0;
};

struct PublishedJumpTable {
    std::uint32_t address = 0;
    std::uint32_t base_register = 0;
    std::uint32_t count = 0;
    std::size_t line = 0;
};

struct PublishedMap {
    std::string profile;
    std::string unit;
    std::string source_sha1; // 40 lower-case hex digits
    std::uint32_t code_start = 0;
    std::uint32_t code_end = 0;                     // exclusive
    std::vector<PublishedFunction> functions;       // sorted, disjoint, inside the code range
    std::vector<std::uint32_t> link_return_entries; // sorted, unique
    std::vector<PublishedName> named;               // sorted by start
    std::vector<PublishedJumpTable> jump_tables;    // sorted by address
    std::filesystem::path path;                     // the file read; empty for parsed text
};

// Parses and checks a published map against its format: only the format's keys, `format` 1, a lower-case
// SHA-1, functions as [start, size] pairs (size > 0, 4-aligned, sorted, disjoint, inside `code`), link-return
// entries inside `code`, [[named]] rows at function starts whose names the runtime's call list knows, and
// [[jump_table]] rows with a non-zero 4-aligned address, base_register 0..31 and count >= 1. Syntax and type
// errors are Error(map_parse), inconsistencies Error(map_invalid), each with "<source>:<line>: ".
PublishedMap parse_published_map(std::string_view text, const std::string& source);

// <root>/<profile>/<unit>.toml
std::filesystem::path published_map_file(const std::filesystem::path& root, std::string_view profile,
                                         std::string_view unit);

// Reads and parses published_map_file(); checks that its `profile` and `unit` match. Throws
// Error(maps_missing) when the file does not exist.
PublishedMap load_published_map(const std::filesystem::path& root, std::string_view profile,
                                std::string_view unit);

// Throws Error(maps_missing) unless the map was published for a unit file with this SHA-1.
void check_published_map_source(const PublishedMap& map, std::string_view source_sha1);

// The name a function without a [[named]] row gets: "FUN_" and its start in eight lower-case hex digits, the
// placeholder the contributor maps carry, so code generated from either kind of map uses the same names.
std::string generated_function_name(std::uint32_t start);

// The recompiler's input for one unit: the functions named from [[named]] or generated_function_name(), the
// link-return entries, and each jump table's `count` entries read as little-endian words at its address from
// `unit_elf` (the unit ELF prepare writes, whose SHA-1 the caller checked). Throws Error(map_invalid) when a
// table's words do not lie inside one of the ELF's load segments with file data, and the ELF reader's errors.
// The result has no file paths; run validate_function_map() on it as on a contributor map.
FunctionMap make_function_map(const PublishedMap& map, std::span<const std::uint8_t> unit_elf);

// A map's functions as the recompiler's CSV (the "Name,Start,End,Size" header, then one row per function in
// address order, "0x%08X" addresses and a decimal size), as tools/ghidra/ExportFunctionMap.java writes it.
std::string function_csv_text(const FunctionMap& map);

// The published maps shipped with the program: data/maps in the working directory or an ancestor of
// `executable_dir` (the build tree lives inside the source tree), else share/dq8/maps next to one of them (an
// install: bin/../share/dq8/maps). Throws Error(maps_missing) when none is found.
std::filesystem::path find_maps_dir(const std::filesystem::path& executable_dir);

} // namespace dq8::core::recomp
