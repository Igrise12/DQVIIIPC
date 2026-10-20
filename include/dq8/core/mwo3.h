// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

// The "MWo3" overlay container: a 64-byte little-endian header followed by the overlay's text and data.
// The game's loader reads the whole file, header included, to load_address, zeroes bss_size bytes after
// the file and calls every pointer in [table_start, table_end) (a C++ static-constructor list); it reads
// only bss_size, table_start and table_end from the header (spike S3). This parser validates all of it so
// the recompiler and the runtime can trust the rest.
//
// Layout (offsets in the file; every word is a little-endian u32):
//   +0x00 magic "MWo3"      +0x04 overlay_id     +0x08 load_address   +0x0C text_size
//   +0x10 data_size         +0x14 bss_size       +0x18 table_start    +0x1C table_end
//   +0x20 name: NUL-terminated, NUL-padded printable ASCII, 32 bytes
//   +0x40 text, then data; the file ends at 64 + text_size + data_size, bss follows in memory.
//
// The text/data split is a link-map boundary, not a code/data boundary: spike S3 found code (constructor
// thunks, vtable targets) inside the data range, so image() is one byte range and text_size is a hint.
namespace dq8::core {

inline constexpr std::size_t mwo3_header_size = 64;
inline constexpr std::size_t mwo3_name_size = 32;
inline constexpr std::uint32_t mwo3_magic = 0x336F574Du; // "MWo3" read as a little-endian u32
inline constexpr std::uint32_t mwo3_max_overlay_id = 64; // sanity bound; the game uses 1..6

// The address range an overlay may occupy: [base, end).
struct Mwo3Window {
    std::uint32_t base = 0;
    std::uint32_t end = 0;
};

struct Mwo3Header {
    std::uint32_t magic = 0;
    std::uint32_t overlay_id = 0;
    std::uint32_t load_address = 0;
    std::uint32_t text_size = 0;
    std::uint32_t data_size = 0;
    std::uint32_t bss_size = 0;
    std::uint32_t table_start = 0;             // absolute address of the first constructor pointer
    std::uint32_t table_end = 0;               // absolute address one past the last pointer
    std::string name;                          // the name field up to its NUL
    std::array<std::uint32_t, 16> raw_words{}; // the header as 16 little-endian words, for `dq8ctl info`

    // Derived from the fields above. All arithmetic was range-checked by the parser.
    std::uint32_t file_size() const noexcept {
        return static_cast<std::uint32_t>(mwo3_header_size) + text_size + data_size;
    }
    std::uint32_t memsz() const noexcept { return file_size() + bss_size; }
    std::uint32_t code_start() const noexcept {
        return load_address + static_cast<std::uint32_t>(mwo3_header_size);
    }
    std::uint32_t data_start() const noexcept { return code_start() + text_size; }
    std::uint32_t bss_start() const noexcept { return load_address + file_size(); }
    std::uint32_t end_address() const noexcept { return bss_start() + bss_size; }
    std::uint32_t table_entry_count() const noexcept { return (table_end - table_start) / 4; }
};

// A parsed container over bytes the caller owns.
struct Mwo3Overlay {
    Mwo3Header header;
    std::span<const std::uint8_t> image; // text then data: the file minus its header; not owned
};

// Parses and validates a header. `header` must hold at least mwo3_header_size bytes (more is fine);
// `file_size` is the container file's size. With a window, load_address must equal window.base and
// 64 + text + data + bss must fit in [window.base, window.end); without one the overlay only has to be
// self-consistent (its own range is used, as `dq8ctl info` does for a lone file).
// Throws Error with code mwo3_magic, mwo3_size, mwo3_id, mwo3_range, mwo3_table or mwo3_name.
Mwo3Header parse_mwo3_header(std::span<const std::uint8_t> header, std::uint64_t file_size,
                             std::optional<Mwo3Window> window = std::nullopt);

// Parses a whole container held in memory. image spans file[64, file.size()).
Mwo3Overlay parse_mwo3(std::span<const std::uint8_t> file, std::optional<Mwo3Window> window = std::nullopt);

// True when the first four bytes spell the container magic.
bool looks_like_mwo3(std::span<const std::uint8_t> head) noexcept;

} // namespace dq8::core
