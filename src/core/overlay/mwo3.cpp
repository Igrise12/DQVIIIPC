// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/mwo3.h"

#include "dq8/core/error.h"
#include "dq8/core/hash.h"

#include <string>

namespace dq8::core {

namespace {

std::uint32_t le32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::string hex32(std::uint32_t v) {
    return "0x" + to_hex(v);
}

} // namespace

bool looks_like_mwo3(std::span<const std::uint8_t> head) noexcept {
    return head.size() >= 4 && le32(head.data()) == mwo3_magic;
}

Mwo3Header parse_mwo3_header(std::span<const std::uint8_t> bytes, std::uint64_t file_size,
                             std::optional<Mwo3Window> window) {
    if (bytes.size() < mwo3_header_size) {
        throw Error(ErrorCode::mwo3_magic, "file is shorter than an MWo3 header (" +
                                               std::to_string(bytes.size()) + " of " +
                                               std::to_string(mwo3_header_size) + " bytes)");
    }
    Mwo3Header h;
    for (std::size_t i = 0; i < h.raw_words.size(); ++i) {
        h.raw_words[i] = le32(bytes.data() + 4 * i);
    }
    h.magic = h.raw_words[0];
    h.overlay_id = h.raw_words[1];
    h.load_address = h.raw_words[2];
    h.text_size = h.raw_words[3];
    h.data_size = h.raw_words[4];
    h.bss_size = h.raw_words[5];
    h.table_start = h.raw_words[6];
    h.table_end = h.raw_words[7];
    if (h.magic != mwo3_magic) {
        throw Error(ErrorCode::mwo3_magic, "not an MWo3 overlay container (magic " + hex32(h.magic) + ")");
    }

    // Sizes: the file is exactly header + text + data, and everything fits in 32-bit guest addresses.
    const std::uint64_t expected_file =
        static_cast<std::uint64_t>(mwo3_header_size) + h.text_size + h.data_size;
    if (expected_file != file_size) {
        throw Error(ErrorCode::mwo3_size,
                    "file is " + std::to_string(file_size) + " bytes but the header declares " +
                        std::to_string(mwo3_header_size) + " + text " + hex32(h.text_size) + " + data " +
                        hex32(h.data_size) + " = " + std::to_string(expected_file));
    }
    const std::uint64_t memsz = expected_file + h.bss_size;
    if (static_cast<std::uint64_t>(h.load_address) + memsz > 0xFFFFFFFFull) {
        throw Error(ErrorCode::mwo3_range, "overlay at " + hex32(h.load_address) + " with memsz " +
                                               std::to_string(memsz) + " bytes crosses the 4 GiB boundary");
    }

    if (h.overlay_id == 0 || h.overlay_id > mwo3_max_overlay_id) {
        throw Error(ErrorCode::mwo3_id, "overlay_id " + std::to_string(h.overlay_id) + " is outside 1.." +
                                            std::to_string(mwo3_max_overlay_id));
    }

    // Window.
    const Mwo3Window w = window.has_value()
                             ? *window
                             : Mwo3Window{h.load_address, static_cast<std::uint32_t>(h.load_address + memsz)};
    if (h.load_address != w.base) {
        throw Error(ErrorCode::mwo3_range, "load_address " + hex32(h.load_address) +
                                               " is not the overlay window base " + hex32(w.base));
    }
    if (w.end < w.base || memsz > static_cast<std::uint64_t>(w.end - w.base)) {
        throw Error(ErrorCode::mwo3_range, "overlay needs " + std::to_string(memsz) +
                                               " bytes (header + text + data + bss) but the window " +
                                               hex32(w.base) + ".." + hex32(w.end) + " holds " +
                                               std::to_string(w.end >= w.base ? w.end - w.base : 0));
    }

    // Constructor table: 4-aligned pointers, start <= end, inside the data range.
    const std::uint32_t data_start = h.data_start();
    const std::uint32_t data_end = data_start + h.data_size;
    if ((h.table_start % 4) != 0 || (h.table_end % 4) != 0) {
        throw Error(ErrorCode::mwo3_table, "constructor table " + hex32(h.table_start) + ".." +
                                               hex32(h.table_end) + " is not 4-byte aligned");
    }
    if (h.table_start > h.table_end) {
        throw Error(ErrorCode::mwo3_table, "constructor table ends (" + hex32(h.table_end) +
                                               ") before it starts (" + hex32(h.table_start) + ")");
    }
    if (h.table_start < data_start || h.table_end > data_end) {
        throw Error(ErrorCode::mwo3_table, "constructor table " + hex32(h.table_start) + ".." +
                                               hex32(h.table_end) + " lies outside the data range " +
                                               hex32(data_start) + ".." + hex32(data_end));
    }

    // Name: NUL-terminated, NUL-padded, non-empty printable ASCII.
    const std::uint8_t* name = bytes.data() + 0x20;
    std::size_t len = 0;
    while (len < mwo3_name_size && name[len] != 0) {
        ++len;
    }
    if (len == mwo3_name_size) {
        throw Error(ErrorCode::mwo3_name, "name is not NUL-terminated within its 32 bytes");
    }
    if (len == 0) {
        throw Error(ErrorCode::mwo3_name, "name is empty");
    }
    for (std::size_t i = 0; i < len; ++i) {
        if (name[i] < 0x20 || name[i] > 0x7E) {
            throw Error(ErrorCode::mwo3_name, "name holds a non-printable byte 0x" +
                                                  to_hex(static_cast<std::uint32_t>(name[i])).substr(6) +
                                                  " at offset " + std::to_string(i));
        }
    }
    for (std::size_t i = len; i < mwo3_name_size; ++i) {
        if (name[i] != 0) {
            throw Error(ErrorCode::mwo3_name,
                        "name is not NUL-padded after its terminator (byte " + std::to_string(i) + ")");
        }
    }
    h.name.assign(reinterpret_cast<const char*>(name), len);
    return h;
}

Mwo3Overlay parse_mwo3(std::span<const std::uint8_t> file, std::optional<Mwo3Window> window) {
    Mwo3Overlay o;
    o.header = parse_mwo3_header(file, file.size(), window);
    o.image = file.subspan(mwo3_header_size);
    return o;
}

} // namespace dq8::core
