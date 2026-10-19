// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A small, strict ISO 9660 (ECMA-119) reader for 2048-byte-sector images.
//
// Scope: level 1/2 volumes with one primary volume descriptor, as PS2 discs are mastered. The directory tree
// is the source of truth; the L and M path tables are parsed and cross-checked against it. Every
// both-byte-order field must agree with itself. Interleaved files, multi-extent files, extended attribute
// records and logical block sizes other than 2048 are refused with a clear error rather than guessed at.
// Joliet (supplementary descriptors), Rock Ridge and UDF are out of scope: the descriptor is noted, the
// extensions are ignored, and names are the plain ISO 9660 identifiers. Compressed images are out of scope.
//
// Everything is bounds-checked against the image size, so a truncated or hostile image produces an Error,
// never an out-of-range read. The reader is not thread-safe: one Image per thread.
namespace dq8::core::iso {

inline constexpr std::uint32_t sector_size = 2048;

struct Entry {
    std::string path;         // "/BIN/TITLE.BIN": '/'-separated, version suffix and trailing '.' removed
    std::string name;         // last component of path ("" for the root)
    std::string identifier;   // the identifier exactly as recorded, e.g. "TITLE.BIN;1" or "DMYDATA.;1"
    std::uint32_t lba = 0;    // first logical block of the extent
    std::uint32_t size = 0;   // data length in bytes
    std::uint8_t flags = 0;   // file flags byte: bit 0 hidden, bit 1 directory, bit 2 associated
    std::uint32_t parent = 0; // index into entries() of the parent directory (the root is its own parent)
    std::uint16_t depth = 0;  // 0 for the root

    bool is_directory() const noexcept { return (flags & 0x02u) != 0; }
};

struct VolumeInfo {
    std::string system_identifier;       // trimmed a-characters, e.g. "PLAYSTATION"
    std::string volume_identifier;       // trimmed
    std::string application_identifier;  // trimmed
    std::uint32_t volume_space_size = 0; // in logical blocks
    std::uint16_t logical_block_size = 0;
    std::uint16_t volume_set_size = 0;
    std::uint16_t volume_sequence_number = 0;
    std::uint32_t path_table_size = 0;
    std::uint32_t path_table_lba_l = 0;
    std::uint32_t path_table_lba_l_optional = 0;
    std::uint32_t path_table_lba_m = 0;
    std::uint32_t path_table_lba_m_optional = 0;
    std::uint32_t root_lba = 0;
    std::uint32_t root_size = 0;
    std::uint32_t path_table_directories = 0; // records in the L path table (== directories in the tree)
    std::uint32_t descriptor_count = 0;       // volume descriptors scanned, terminator included
    bool has_boot_record = false;             // a type 0 descriptor exists (ignored)
    bool has_supplementary = false;           // a type 2 descriptor exists (Joliet; ignored)
};

class Image {
  public:
    static Image open(const std::filesystem::path& path);
    // For tests and fuzzing: an image held entirely in memory.
    static Image from_bytes(std::vector<std::uint8_t> bytes);

    Image(Image&&) noexcept;
    Image& operator=(Image&&) noexcept;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    ~Image();

    std::uint64_t image_size() const noexcept;     // bytes in the file
    std::uint32_t image_sectors() const noexcept;  // image_size() / 2048, rounded down
    std::uint32_t volume_sectors() const noexcept; // the primary descriptor's volume space size
    const VolumeInfo& volume() const noexcept;

    // Every entry in depth-first order, the root first. Directories appear before their contents.
    const std::vector<Entry>& entries() const noexcept;

    // Case-insensitive lookup. Accepts '/' or '\' separators, an optional leading separator, an optional
    // ";1" version and an optional trailing '.' on the last component. Returns nullptr when absent.
    const Entry* find(std::string_view path) const noexcept;
    // Like find(), but throws Error(ErrorCode::iso_not_found).
    const Entry& require(std::string_view path) const;

    // Reads out.size() bytes of the entry's data starting at `offset`. Throws iso_range when the span does
    // not lie within the entry, iso_truncated when it does not lie within the image.
    void read(const Entry& entry, std::uint64_t offset, std::span<std::uint8_t> out) const;
    // Reads `count` whole sectors starting at `lba`. out.size() must equal count * sector_size.
    void read_sectors(std::uint32_t lba, std::uint32_t count, std::span<std::uint8_t> out) const;
    // Reads a whole entry, refusing (iso_range) anything larger than max_bytes.
    std::vector<std::uint8_t> read_all(const Entry& entry, std::uint64_t max_bytes = 64u << 20) const;

    // The lookup key used by find(): uppercase, '/'-separated, no leading separator, versions and trailing
    // dots stripped. Exposed so callers can compare disc paths the same way.
    static std::string normalize_path(std::string_view path);

  private:
    struct Impl;
    explicit Image(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace dq8::core::iso
