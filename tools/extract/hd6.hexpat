// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// ImHex pattern for the HD6 archive index (DATA.HD6 / DATA2.HD6 beside DATA.DAT / DATA2.DAT on the
// Dragon Quest VIII disc). Layout authored from observation and from the game's reader
// (Docs/formats/hd6.md); it carries no values from the disc. Open the .HD6 file in ImHex and load
// this pattern.
//
// Three sections follow a 52-byte header: a NUL-terminated string table (the dictionary of path
// fragments), a table of NUL-terminated path records (each a sequence of dictionary indices), and
// an array of 8-byte entries. Entry i's record gives its full path; its sector offset and size
// locate the data in the .DAT file.

#pragma endian little
#pragma magic [ 48 44 36 00 ] @ 0x00
#pragma pattern_limit 2000000

#include <std/io.pat>
#include <std/mem.pat>

struct Header {
    char magic[4];        // "HD6\0"
    u32 dict_offset;      // string table: always 0x34, right after this header
    u32 dict_size;        // bytes, padded with NULs to a multiple of 4
    u32 dict_count;       // dictionary strings (index 0 and the last one are empty)
    u32 reserved0;        // 0 on disc, not read by the game
    u32 paths_offset;     // path-record table (dict_offset + dict_size)
    u32 paths_size;       // bytes, a multiple of 4; not read by the game
    u32 reserved1;        // 0 on disc, not read by the game
    u32 size_unit;        // 16: entry sizes are stored in units of this many bytes
    u32 entry_count;      // entries, including the terminating empty-name entry
    u32 entries_offset;   // paths_offset + paths_size
    u32 reserved2;        // 0 on disc, not read by the game
    u32 file_size;        // == size of the .HD6 file; not read by the game (it uses lseek)
};

// One NUL-terminated dictionary string.
struct DictString {
    char text[];
} [[inline]];

// One path record: a NUL-terminated sequence of dictionary indices. A byte below 0x80 is an index
// on its own; a byte b >= 0x80 followed by byte n encodes index (b & 0x7F) | (n << 7). The full
// path is the concatenation of the indexed fragments: the leading name, then fragments that each
// start with their separator ('\' on disc, rewritten to '/' in memory; '.', '_' or '-').
struct PathRecord {
    u8 code[while(std::mem::read_unsigned($, 1) != 0)];
    u8 terminator;
} [[inline]];

// One 8-byte entry, a little-endian 64-bit bitfield (least significant field first), exactly as
// the game's lookup unpacks it.
bitfield Entry {
    path_offset : 18;    // byte offset of the PathRecord inside the path table
    sector      : 22;    // offset of the data in the .DAT file, in 2,048-byte sectors
    size_units  : 24;    // size of the data in bytes / size_unit (16)
} [[format("format_entry")]];

fn format_entry(Entry e) {
    return std::format("path@{:#x} sector {} size {}", e.path_offset, u64(e.sector),
                       u64(e.size_units) * 16);
};

Header header @ 0x00;
DictString dictionary[header.dict_count] @ header.dict_offset;
PathRecord paths[header.entry_count] @ header.paths_offset;
Entry entries[header.entry_count] @ header.entries_offset;
