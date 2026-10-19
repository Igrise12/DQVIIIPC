// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Region profiles: data/regions/<serial>.toml. One per supported disc. A profile records only what the
// policy in architecture.md §14 allows (names, sizes, addresses, hashes), and the loader validates the
// schema with file:line errors so a bad edit fails before any disc is read. data/README.md documents the
// schema; schema_version is bumped when a field changes meaning.
namespace dq8::core {

inline constexpr int region_profile_schema_version = 1;

struct ProfileFile {
    std::string path;       // disc path relative to the root, '/'-separated, e.g. "MODULES/PADMAN.IRX"
    std::string role;       // "iop_module", "iop_image", "system_cnf" or "other"
    std::uint64_t size = 0; // 0 when not yet recorded
    std::string sha1;       // 40 lowercase hex digits, or empty when not yet recorded
    std::string crc32;      // 8 lowercase hex digits, or empty when not yet recorded
};

struct ProfileOverlay {
    std::string name;                // our label, e.g. "CASINO"
    std::uint32_t id = 0;            // the id the container header carries (1-based)
    std::string path;                // disc path, e.g. "BIN/CASINO.BIN"
    std::uint32_t segment_memsz = 0; // memsz of the matching zero-filesize PT_LOAD segment in the boot ELF
    std::uint64_t size = 0;
    std::string sha1;
    std::string crc32;
};

struct RegionProfile {
    int schema_version = 0;
    std::string id;     // "SLUS_21207": the directory name under baserom/ and the key used everywhere
    std::string serial; // "SLUS-21207"
    std::string title;
    std::string region;     // "NTSC-U"
    std::string video_mode; // "NTSC"
    std::uint64_t image_size = 0;
    std::uint32_t sector_size = 0;

    struct Boot {
        std::string path; // "SLUS_212.07"
        std::uint64_t size = 0;
        std::string sha1;  // required
        std::string crc32; // required
        std::uint32_t entry = 0;
        std::uint32_t load_address = 0;
        std::uint32_t main_memsz = 0;
    } boot;

    struct OverlayWindow {
        std::uint32_t base = 0;
        std::uint32_t end = 0;
        std::string container; // "MWo3"
        std::uint32_t header_size = 0;
    } overlay_window;

    std::vector<ProfileOverlay> overlays; // in segment order (by id)
    std::vector<ProfileFile> files;
    std::vector<std::string> not_extracted; // disc paths; a trailing '/' marks a directory
    std::filesystem::path source;           // the file this profile was loaded from
};

// Loads and validates one profile. Throws Error(profile_parse) for TOML errors and Error(profile_schema)
// for schema violations; both messages start with "<file>:<line>:".
RegionProfile load_region_profile(const std::filesystem::path& file);

// Loads every *.toml in `dir`, sorted by file name. Throws io_open when the directory is missing or holds
// no profiles, and profile_conflict when two profiles share an id or a boot hash.
std::vector<RegionProfile> load_region_profiles(const std::filesystem::path& dir);

// The profile whose boot SHA-1 equals `sha1_hex` (case-insensitive), or nullptr.
const RegionProfile* find_profile_by_boot_sha1(std::span<const RegionProfile> profiles,
                                               std::string_view sha1_hex);

} // namespace dq8::core
