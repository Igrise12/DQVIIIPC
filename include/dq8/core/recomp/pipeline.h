// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/process.h"
#include "dq8/core/recomp/function_map.h"
#include "dq8/core/recomp/module_build.h"
#include "dq8/core/recomp/recomp_driver.h"
#include "dq8/core/recomp/units.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Stages 3 to 6 of the setup pipeline (architecture.md §3) over a work directory, for `dq8ctl recomp` and the
// launcher. The stages chain: prepare -> generate -> build, each taking the previous one's report, and each
// is idempotent and resumable: prepare rewrites a file only when its bytes change, generate skips a unit
// whose recorded input key (recompiler, config, unit ELF and map hashes) still matches, and build runs an
// incremental CMake/Ninja build. Everything under the work directory is derived from the user's disc and
// stays local; prepare writes a ".gitignore" of "*" at its root.
namespace dq8::core {
struct VerifiedImage;
}

namespace dq8::core::recomp {

// The work directory:
//   units/units.toml            what prepare wrote: source, per unit kind, ranges, hashes and map counts
//   units/<unit>.elf            the unit ELF (main: the boot ELF unchanged; overlays: make_overlay_unit_elf)
//   units/<unit>.toml           the recompiler config
//   units/<unit>.csv            the recompiler's function list, made from a published map (contributor maps
//                               are read where they are)
//   generated/<unit>/           the recompiler's output
//   build/<unit>/               the module build tree
//   modules/<profile>/          the modules and build-manifest.toml
//   logs/<unit>.<stage>.log     full tool output of the last run of a stage
//   stamps/<unit>.generate.toml the generate stage's input key, generator and report counts
class WorkLayout {
  public:
    explicit WorkLayout(std::filesystem::path root) : root_(std::move(root)) {}
    const std::filesystem::path& root() const noexcept { return root_; }
    std::filesystem::path units_file() const { return root_ / "units" / "units.toml"; }
    std::filesystem::path unit_elf(std::string_view unit) const {
        return root_ / "units" / (std::string(unit) + ".elf");
    }
    std::filesystem::path unit_config(std::string_view unit) const {
        return root_ / "units" / (std::string(unit) + ".toml");
    }
    std::filesystem::path unit_map(std::string_view unit) const {
        return root_ / "units" / (std::string(unit) + ".csv");
    }
    std::filesystem::path generated_dir(std::string_view unit) const { return root_ / "generated" / unit; }
    std::filesystem::path build_dir(std::string_view unit) const { return root_ / "build" / unit; }
    std::filesystem::path modules_dir(std::string_view profile) const { return root_ / "modules" / profile; }
    std::filesystem::path build_manifest(std::string_view profile) const {
        return modules_dir(profile) / "build-manifest.toml";
    }
    std::filesystem::path log_file(std::string_view unit, std::string_view stage) const {
        return root_ / "logs" / (std::string(unit) + "." + std::string(stage) + ".log");
    }
    std::filesystem::path generate_record(std::string_view unit) const {
        return root_ / "stamps" / (std::string(unit) + ".generate.toml");
    }

  private:
    std::filesystem::path root_;
};

// One unit's ELF and the SHA-1 that identifies where it came from (the boot file, or the overlay container).
struct UnitImage {
    std::vector<std::uint8_t> elf;
    std::string source_sha1;
};

// Where the code units come from.
struct UnitSource {
    std::string profile_id;      // names modules/<profile>/ and goes into every module descriptor
    std::string identity;        // the boot file's SHA-1; "dev" for unit ELFs given directly
    std::vector<CodeUnit> units; // in pipeline order
    std::function<UnitImage(const CodeUnit&)> load;
};

// The verified disc: code_units(verified), the boot ELF as main's unit ELF, make_overlay_unit_elf() for the
// overlays (window from the profile, e_flags from the boot ELF).
UnitSource disc_units(const VerifiedImage& verified);

// Developer mode (synthetic tests, experiments): unit ELFs given as (name, path). "main" is the main unit
// with the ELF's entry point; any other name is an overlay, its overlay_id read from the MWo3 header at the
// start of its executable segment (0 when there is none). The code range is the first executable load segment
// with data. Throws invalid_argument for a repeated or malformed name, elf_format for an unreadable ELF.
UnitSource elf_units(const std::vector<std::pair<std::string, std::filesystem::path>>& elfs,
                     std::string profile_id);

struct PipelineOptions {
    std::filesystem::path work_dir;
    std::filesystem::path maps_dir;           // prepare: contributor maps (<unit>.csv, ...); empty: published
    std::filesystem::path published_maps_dir; // prepare: the <profile>/<unit>.toml root (find_maps_dir())
    std::vector<std::string> units;           // unit names; empty: every unit
    std::filesystem::path ps2_recomp;         // generate: the recompiler (a path or a name in PATH)
    std::filesystem::path cmake = "cmake";    // build
    std::string cmake_generator = "Ninja";    // build
    std::filesystem::path module_source_dir;  // build: the module template (cmake/module)
    std::filesystem::path abi_include_dir;    // build: the directory holding dq8/abi/
    std::vector<std::string> extra_cmake_args;
    unsigned jobs = 0;       // build: 0 picks default_build_jobs()
    bool force = false;      // generate: ignore the recorded key and rerun
    bool pch = true;         // build: DQ8_MODULE_PCH
    bool debug_info = false; // build: DQ8_MODULE_DEBUG_INFO
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(const std::string& unit, const std::string& message)> on_progress;
    std::function<void(OutputStream, std::string_view)> on_tool_output; // the tools' raw output
};

struct PreparedUnit {
    CodeUnit unit;
    std::string source_sha1;
    std::filesystem::path elf;
    std::filesystem::path config;
    std::filesystem::path map_csv;       // the function list the recompiler reads
    std::filesystem::path published_map; // the published map it came from; empty for contributor maps
    std::size_t functions = 0;
    std::size_t stubs = 0;
    std::size_t entry_points = 0;
    std::size_t jump_tables = 0;
    bool elf_written = false;    // false: the file already held these bytes
    bool config_written = false; // likewise
    std::vector<std::string> warnings;
};

struct PrepareReport {
    std::string profile_id;
    std::string source_identity; // UnitSource::identity
    std::filesystem::path work_dir;
    std::filesystem::path units_file;
    std::vector<PreparedUnit> units; // the selected units, in pipeline order
};

struct GeneratedUnit {
    std::string name;
    bool skipped = false;  // the recorded input key matched: nothing ran, `result` holds the recorded counts
    GenerateResult result; // timings and file counts of this run (or the recorded ones)
    std::filesystem::path output_dir;
    std::filesystem::path log_file;
};

struct GenerateReport {
    std::filesystem::path ps2_recomp; // resolved
    std::string generator_id;         // "ps2_recomp sha1:<12 hex>"
    std::vector<GeneratedUnit> units; // as in the prepare report
};

struct BuiltUnit {
    std::string name;
    ModuleBuildResult result;
    std::filesystem::path module; // the copy under modules/<profile>/
    bool module_updated = false;  // false: the copy already held these bytes
    std::filesystem::path log_file;
};

struct BuildReport {
    std::filesystem::path modules_dir;
    std::filesystem::path manifest;
    unsigned jobs = 0;
    std::vector<BuiltUnit> units;
};

// The setup's `maps` stage: the published map of every unit of the verified disc (all of them when `units` is
// empty), each checked against the SHA-1 the region profile records for the unit's file (the boot file's for
// main) and against the unit's code range. Throws Error(maps_missing) for a unit without a map or with a map
// of another file, and the published maps' map_parse and map_invalid errors.
std::vector<PublishedMap> require_published_maps(const std::filesystem::path& maps_root,
                                                 const VerifiedImage& verified,
                                                 const std::vector<std::string>& units = {});

// Stages 3-4: writes every selected unit's ELF and recompiler config from the source and the maps, then
// units/units.toml. With maps_dir set, the contributor maps there; else the published map of each unit under
// published_maps_dir, checked against the unit file's SHA-1 (maps_missing), with its jump tables' entries
// read from the unit ELF and its function list written to units/<unit>.csv. Throws the map, container and I/O
// errors of the pieces, invalid_argument for an unknown unit or when neither map directory is set,
// image_mismatch when a file read back differs from what verify hashed.
PrepareReport prepare_units(const UnitSource& source, const PipelineOptions& options);

// Stage 5: runs the recompiler on every prepared unit whose inputs changed. Throws tool_missing when the
// recompiler cannot be found, and the driver's errors.
GenerateReport generate_units(const PrepareReport& prepared, const PipelineOptions& options);

// Stage 6: builds every generated unit's module, copies it to modules/<profile>/ and updates
// build-manifest.toml (input hashes, generator, job count, timings). A module that comes out the same from
// the same inputs keeps its earlier row, so a re-run that changed nothing leaves the manifest byte for byte.
// Throws the module build's errors.
BuildReport build_units(const PrepareReport& prepared, const GenerateReport& generated,
                        const PipelineOptions& options);

} // namespace dq8::core::recomp
