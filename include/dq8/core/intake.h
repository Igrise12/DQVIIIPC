// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/elf_headers.h"
#include "dq8/core/iso9660.h"
#include "dq8/core/mwo3.h"
#include "dq8/core/region_profile.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Disc intake: stages 1 and 2 of the setup pipeline (architecture.md §3). verify_image() identifies the
// disc from its boot file and cross-checks it against a region profile; extract() copies the boot file,
// the overlays and the listed modules into baserom/<profile id>/, idempotently. Both throw Error.
namespace dq8::core {

struct SystemCnf {
    std::string boot2;     // the BOOT2 value as written, e.g. "cdrom0:\SLUS_212.07;1"
    std::string boot_path; // the disc path BOOT2 names, without device and version: "SLUS_212.07"
    std::string ver;       // VER, if present
    std::string vmode;     // VMODE, if present
    std::vector<std::pair<std::string, std::string>> entries; // every "KEY = VALUE" line, keys uppercased
};

// Parses SYSTEM.CNF text. Throws Error(system_cnf) when BOOT2 is missing or not a cdrom0: path.
SystemCnf parse_system_cnf(std::string_view text);

struct FileDigest {
    std::uint64_t size = 0;
    std::string sha1;  // lowercase hex
    std::string crc32; // lowercase hex
};

using ProgressFn = std::function<void(std::string_view what, std::uint64_t done, std::uint64_t total)>;

// Hashes an entry's data, streaming it in 1 MiB chunks.
FileDigest digest_entry(const iso::Image& image, const iso::Entry& entry, const ProgressFn& progress = {});
// Hashes a file on disk the same way.
FileDigest digest_file(const std::filesystem::path& file);

struct VerifiedImage {
    std::filesystem::path image_path;
    std::uint64_t image_size = 0;
    std::uint32_t volume_sectors = 0;
    RegionProfile profile;
    SystemCnf system_cnf;
    std::string boot_path; // as found on the disc, e.g. "/SLUS_212.07"
    FileDigest boot;
    ElfHeaders elf;
    std::vector<Mwo3Header> overlay_headers; // one per profile.overlays, in that order, read from the disc
    std::vector<std::string> warnings; // facts that differ from the profile without disqualifying the disc
};

// Opens the image, reads SYSTEM.CNF, hashes the boot file and matches it against `profiles`, then
// cross-checks the ELF headers and the listed file sizes, and parses every overlay's container header
// from the image, checking it against its profile row and its ELF segment. Throws Error with code
// system_cnf, boot_file_missing, unknown_image (message carries the observed hash and size) or
// image_mismatch (also for an overlay that is not a valid container).
VerifiedImage verify_image(const std::filesystem::path& image_path, std::span<const RegionProfile> profiles,
                           const ProgressFn& progress = {});

enum class FileStatus {
    extracted,     // written in this run
    up_to_date,    // already present with the recorded hash; skipped
    hash_mismatch, // written, but its hash differs from the profile's recorded value
};

struct ExtractedFile {
    std::string disc_path;          // "BIN/TITLE.BIN"
    std::string role;               // "boot", "overlay", or the [[files]] role
    std::filesystem::path out_path; // where it was written
    FileDigest digest;              // of the data now on disk
    std::string expected_sha1;      // from the profile, or empty
    FileStatus status = FileStatus::extracted;
};

struct ExtractOptions {
    bool force = false;        // re-extract even when the manifest says a file is up to date
    bool verify_hashes = true; // compare extracted files against the hashes recorded in the profile
    bool emit_profile = false; // fill ExtractReport::profile_fragment
    ProgressFn progress;
};

struct ExtractReport {
    std::filesystem::path root;        // the out_root given, e.g. baserom/
    std::filesystem::path profile_dir; // root / profile id
    std::filesystem::path manifest;    // profile_dir / manifest.toml
    std::vector<ExtractedFile> files;
    std::string profile_fragment;      // TOML rows with sizes and hashes, when emit_profile was set
    std::vector<std::string> warnings; // e.g. a malformed manifest that was ignored

    std::size_t count(FileStatus status) const noexcept;
    bool has_mismatch() const noexcept { return count(FileStatus::hash_mismatch) != 0; }
};

// Extracts the boot file, overlays and [[files]] of the verified profile into out_root/<profile id>/,
// streaming and hashing in 1 MiB chunks, and records them in out_root/<profile id>/manifest.toml. Files
// whose manifest record and on-disk hash already match are skipped unless `force`. Writes out_root/.gitignore
// containing "*". Never touches the profile's not_extracted paths. Hash mismatches are reported in the
// returned files, not thrown, so the caller can show all of them.
ExtractReport extract(const VerifiedImage& verified, const std::filesystem::path& out_root,
                      const ExtractOptions& options = {});

// The [boot], [[overlays]] and [[files]] rows of a region profile with the sizes and hashes observed in
// `report`, for pasting into data/regions/<id>.toml.
std::string format_profile_fragment(const VerifiedImage& verified, const ExtractReport& report);

} // namespace dq8::core
