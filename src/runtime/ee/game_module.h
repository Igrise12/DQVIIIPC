// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/abi/game_module.h"
#include "runtime/platform/dynlib.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Game modules (architecture §2.1, §13.3): one shared library per code unit, built on the user's machine,
// exporting only dq8_game_module_describe. The host loads each one, reads its descriptor and checks it
// against what it expects of that unit (a UnitSpec, which the game layer derives from the region profile)
// before any of its code runs.
namespace dq8::rt {

// The library base name of every unit module: dq8_unit_<unit name>.
inline constexpr std::string_view module_name_prefix = "dq8_unit_";

// What the host expects of one unit. Empty strings and a zero range_end mean "not checked" (developer mode).
struct UnitSpec {
    std::string name;                   // the unit name the descriptor must carry, e.g. "main"
    std::uint32_t kind = DQ8_UNIT_MAIN; // enum dq8_unit_kind
    std::uint32_t overlay_id = 0;       // 0 for the main unit
    std::string profile_id;             // region profile id
    std::string source_sha1;            // 40 hex digits, compared case-insensitively
    std::uint32_t entry_point = 0; // main unit: the guest entry, which must have a table slot; overlays: 0
    std::uint32_t range_begin = 0; // the table must lie in [range_begin, range_end)
    std::uint32_t range_end = 0;
};

// Every problem with `desc` measured against `spec`, one sentence each; empty when the module is usable. A
// bad magic, ABI version or descriptor size is reported alone, because the rest of the layout cannot be
// trusted. Safe on any pointer the module returned, including null and null strings.
std::vector<std::string> validate_descriptor(const dq8_game_module_desc* desc, const UnitSpec& spec);

class ModuleError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class GameModule {
  public:
    // Loads the library and calls its describe function. Throws DynlibError when the library does not load
    // and ModuleError when it has no dq8_game_module_describe export or the export returns null. The
    // descriptor is not validated here.
    static GameModule open(const std::filesystem::path& file);

    const dq8_game_module_desc& descriptor() const noexcept { return *desc_; }
    const std::filesystem::path& path() const noexcept { return path_; }

  private:
    GameModule(DynamicLibrary library, const dq8_game_module_desc* desc, std::filesystem::path path)
        : library_(std::move(library)), desc_(desc), path_(std::move(path)) {}

    DynamicLibrary library_;
    const dq8_game_module_desc* desc_ = nullptr;
    std::filesystem::path path_;
};

// <dir>/<platform file name of dq8_unit_<unit>>, e.g. dir/libdq8_unit_main.so.
std::filesystem::path module_path(const std::filesystem::path& dir, std::string_view unit_name);

// The unit name a module file name encodes ("libdq8_unit_battle.so" -> "battle"), or empty when the name is
// not a unit module's on this platform.
std::string unit_name_from_module_file(const std::filesystem::path& file);

// Every unit module in `dir`, sorted by file name. Throws std::filesystem::filesystem_error when dir is
// unreadable.
std::vector<std::filesystem::path> find_module_files(const std::filesystem::path& dir);

struct LoadedUnit {
    UnitSpec spec;
    GameModule module;
};

struct ModuleSet {
    std::vector<LoadedUnit> units;     // the units that loaded and passed validation, in spec order
    std::vector<std::string> problems; // "<unit>: <problem>" for every problem of every unit
    bool ok() const noexcept { return problems.empty(); }
};

// Loads module_path(dir, spec.name) for every spec and validates it. Missing files, load failures and
// descriptor problems are collected, never thrown, so one run reports all of them.
ModuleSet load_modules(std::span<const UnitSpec> specs, const std::filesystem::path& dir);

// A nul-terminated descriptor string as a view, "" for null.
inline std::string_view descriptor_string(const char* s) noexcept {
    return s != nullptr ? std::string_view(s) : std::string_view();
}

} // namespace dq8::rt
