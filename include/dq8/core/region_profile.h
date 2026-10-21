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
    std::string name;                // our label, e.g. "CASINO"; also its entry in name_table_order
    std::uint32_t id = 0;            // the overlay_id the container header carries (1-based)
    std::uint32_t index = 0;         // the loader's index: position of `name` in name_table_order
    std::uint32_t segment_index = 0; // position of the matching PT_LOAD among the boot ELF's load segments
                                     // in program-header order (0 is the main image, so this is >= 1)
    std::string path;                // disc path, e.g. "BIN/CASINO.BIN"
    std::uint32_t segment_memsz = 0; // memsz of that zero-filesize PT_LOAD segment
    // Header integrity values: what the container's header must declare. The loader checks that
    // header_size + text_size + data_size + bss_size == segment_memsz and, when `size` is recorded, that
    // header_size + text_size + data_size == size.
    std::uint32_t text_size = 0;
    std::uint32_t data_size = 0;
    std::uint32_t bss_size = 0;
    std::uint32_t table_entries = 0; // pointers in [table_start, table_end)
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
        // Optional: the static-constructor pointer table crt0 walks, [start, end) (0, 0 when not recorded).
        // Its entries are function starts the function-map export must contain (tools/ghidra/export_maps.sh).
        std::uint32_t ctor_table_start = 0;
        std::uint32_t ctor_table_end = 0;
    } boot;

    // The overlay window and the loader facts from spike S3. Addresses other than base/end lie in the
    // main program (code or bss) and are what the runtime overrides or watches; nothing in src/runtime
    // may hardcode them (architecture.md §4.3).
    struct OverlayWindow {
        std::uint32_t base = 0;
        std::uint32_t end = 0;
        std::string container;              // "MWo3"
        std::uint32_t header_size = 0;      // bytes of header resident at base (64 for MWo3)
        std::uint32_t code_offset = 0;      // where the text begins, relative to base (== header_size)
        std::uint32_t window_pointer = 0;   // data word holding `base`; the loader's destination
        std::uint32_t loader = 0;           // int load(const char* path, void* dest)
        std::uint32_t loader_hook = 0;      // the empty function the loader calls after the read
        std::uint32_t load_by_index = 0;    // int load_overlay(int index)
        std::uint32_t name_table = 0;       // the pointer table of disc paths load_by_index indexes
        std::uint32_t lsn_cache = 0;        // the start-sector cache filled at init, one word per index
        std::uint32_t ctor_walker = 0;      // walks [table_start, table_end) calling each pointer
        std::uint32_t flush_cache_stub = 0; // the FlushCache kernel-call stub
        std::uint32_t mode_variable = 0;    // the game-mode variable the main loop switches on
        std::vector<std::string> name_table_order; // overlay names in name_table order (index order)
    } overlay_window;

    // The IOP modules the game loads and the native services that stand in for each
    // (Docs/milestones/M3-boots.md, ADR-0011). Optional: empty when the profile has no [iop] table.
    struct IopModule {
        std::string name;                  // file basename without extension, upper case: "PADMAN"
        std::vector<std::string> services; // runtime catalogue names: "padman.v1", "null:0x123456", ...
    };
    struct Iop {
        std::string reset_image; // disc path of the IOP update image the game resets the IOP with
        std::vector<IopModule> modules;
    } iop;

    // A scene timeline that runs alongside a movie on the scene's own frame counter, movie or not (M3
    // bring-up: TITLE's flyover). While `overlay` is resident and the stage variable holds `stage_value`, the
    // scene steps `counter`, a signed 32-bit word, up from 0; `skip_to` is the step the game itself jumps to
    // when the player skips the movie. dq8-game --movies skip writes it once the counter is above 0
    // (game/movie_timelines.h, ADR-0013, proposed).
    struct MovieTimeline {
        std::string name;    // our label, e.g. "title-flyover": [a-z0-9-], unique
        std::string overlay; // an overlay name
        std::uint32_t stage_variable = 0;
        std::uint32_t stage_width = 0; // bytes: 1, 2 or 4 (little-endian, unsigned)
        std::uint32_t stage_value = 0;
        std::uint32_t counter = 0; // 4-byte aligned
        std::int32_t skip_to = 0;  // > 0
    };

    // The movie player entry points the runtime overrides to skip movies (M3: playback is M4). Optional and
    // marked "to be confirmed" in the profile until bring-up proves them.
    struct Movie {
        std::vector<std::uint32_t> player_entries; // main-program functions the scenes call to play a movie
        std::uint32_t finished_query = 0; // the player's "movie finished?" function (0: not recorded)
        std::string scene_unit;           // the overlay holding the scene functions, e.g. "TITLE"
        std::vector<std::uint32_t> scene_functions; // fallback override targets inside the overlay window
        std::vector<MovieTimeline> timelines;       // [[movie.timelines]], in the file's order
        // [movie.sce_mpeg] (M4, ADR-0015): the SDK entry points the movie HLE overrides or hooks, all in the
        // main program; absent (create == 0) when the profile has no table.
        struct SceMpeg {
            std::uint32_t create = 0;
            std::uint32_t delete_ = 0;
            std::uint32_t add_callback = 0;
            std::uint32_t add_str_callback = 0;
            std::uint32_t demux_pss_ring = 0;
            std::uint32_t get_picture = 0;
            std::uint32_t get_picture_inner = 0;
            std::uint32_t is_end = 0;
            std::uint32_t reset = 0;
            std::uint32_t ipu_init = 0;
            std::uint32_t video_stream_callback = 0;
            bool present() const noexcept { return create != 0; }
        } sce_mpeg;
    } movie;

    // What "the title screen" is (dq8-game --until title; M3 bring-up): the overlay that must be resident and
    // a guest variable that holds `value` on that screen. Optional: variable 0 when the profile has no
    // [title].
    struct Title {
        std::string overlay; // an overlay name
        std::uint32_t variable = 0;
        std::uint32_t width = 0; // bytes: 1, 2 or 4 (little-endian, unsigned)
        std::uint32_t value = 0;
    } title_screen;

    std::vector<ProfileOverlay> overlays; // in the file's order; every overlay has a unique id and index
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
