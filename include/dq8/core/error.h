// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace dq8::core {

// Stable error codes. The CLI maps them to exit statuses and prints their names in --json output, so a code
// may be added but never renamed or reused. Messages are for people; codes are for programs.
enum class ErrorCode {
    io_open,               // the file could not be opened
    io_read,               // a read failed or came up short
    io_write,              // a write, rename or directory creation failed
    iso_descriptor,        // the volume descriptor set is missing or malformed (not an ISO 9660 image)
    iso_truncated,         // an extent or sector lies beyond the end of the image
    iso_endian_mismatch,   // a both-byte-order field disagrees with itself (corruption)
    iso_record,            // a directory record is malformed
    iso_unsupported,       // a feature we deliberately do not read (interleave, multi-extent, ...)
    iso_cycle,             // a directory extent was reached twice
    iso_depth,             // the directory tree is deeper than the limit
    iso_path_table,        // the path table and the directory tree disagree
    iso_not_found,         // find() miss, reported by callers that require the entry
    iso_range,             // a read outside an entry or the image
    elf_format,            // not an ELF32 little-endian file, or its headers are malformed
    profile_parse,         // a region profile is not valid TOML
    profile_schema,        // a region profile does not match the schema
    profile_conflict,      // two profiles claim the same id or boot hash
    system_cnf,            // SYSTEM.CNF is missing or unparsable
    boot_file_missing,     // SYSTEM.CNF names a boot file the image does not contain
    unknown_image,         // the boot file's hash matches no region profile
    image_mismatch,        // a profile matched the hash but another fact disagrees (ELF headers, ...)
    extract_hash_mismatch, // an extracted file's hash differs from the profile's recorded hash
    mwo3_magic,            // not an MWo3 overlay container (bad magic or shorter than a header)
    mwo3_size,             // the file size is not 64 + text_size + data_size
    mwo3_id,               // overlay_id is 0 or larger than mwo3_max_overlay_id
    mwo3_range,            // load_address is not the window base, or the overlay does not fit the window
    mwo3_table,            // the constructor table is misaligned, inverted or outside the data range
    mwo3_name,             // the name is not NUL-terminated, NUL-padded, non-empty printable ASCII
    elf_layout,            // ElfWriter input is inconsistent: overlapping, misaligned or unnamed sections
    process_spawn,         // a child program could not be started (not found, not executable)
    process_timeout,       // a child program ran past its time limit and was stopped
    process_cancelled,     // a child program was stopped because the caller cancelled
    map_parse,             // a function map, jump-table fragment or stats file is malformed (file:line)
    map_invalid,           // a function map is well-formed but inconsistent (overlap, outside its unit, ...)
    recomp_failed,         // the recompiler exited with an error status or printed no report
    module_build_failed,   // configuring or building a game module failed
    tool_missing,          // a required program or template (ps2_recomp, cmake, cmake/module) was not found
    work_stale,            // the work directory lacks a stage's inputs or was prepared for another disc
    invalid_argument,      // a caller-supplied option names something that does not exist (a unit, ...)
    // M5 (setup, settings, launcher):
    settings_parse,       // a settings schema or settings file is not valid TOML
    settings_invalid,     // a setting is unknown, of the wrong type or out of range
    toolchain_incomplete, // the C++ compiler, cmake or ninja the setup needs was not found
    download_failed,      // the bundled toolchain could not be downloaded or failed its checksum
    insufficient_space,   // the work or data directory lacks the free space the setup needs
    setup_cancelled,      // the user cancelled the setup; running it again resumes
    smoke_failed,         // the freshly built game did not reach the title screen in the smoke test
    maps_missing,         // no function maps (published or contributor) exist for a unit of this disc
    registry_parse,       // the build registry (installs.toml) is malformed
};

// Stable kebab-case spelling of a code, e.g. "unknown-image". Used in --json output.
std::string_view to_string(ErrorCode code) noexcept;

// True for codes that mean "this image is not (or no longer) the disc the profile describes", as opposed to
// an I/O failure, a bad profile or a usage error. The CLI exits 1 for these and 2 for everything else.
bool is_mismatch(ErrorCode code) noexcept;

class Error : public std::runtime_error {
  public:
    Error(ErrorCode code, const std::string& message) : std::runtime_error(message), code_(code) {}
    ErrorCode code() const noexcept { return code_; }

  private:
    ErrorCode code_;
};

} // namespace dq8::core
