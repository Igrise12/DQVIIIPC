// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Builds a minimal little-endian ELF32 executable in memory: a header and one PT_LOAD program header per
// segment, the segments' bytes after them, no sections. Enough for the runtime's loader tests and the
// contract unit's ELF; the bytes are whatever the test puts in, never anything from a game.
namespace test_elf {

struct Segment {
    std::uint32_t vaddr = 0;
    std::vector<std::uint8_t> bytes; // filesz = bytes.size()
    std::uint32_t memsz = 0;         // >= bytes.size()
    std::uint32_t flags = 7;         // PF_R | PF_W | PF_X
};

inline void put16(std::vector<std::uint8_t>& out, std::size_t at, std::uint16_t v) {
    out[at] = static_cast<std::uint8_t>(v);
    out[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

inline void put32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

inline std::vector<std::uint8_t> build(std::uint32_t entry, const std::vector<Segment>& segments,
                                       std::uint16_t machine = 8 /* EM_MIPS */,
                                       std::uint16_t type = 2 /* ET_EXEC */) {
    constexpr std::size_t ehsize = 52;
    constexpr std::size_t phentsize = 32;
    const std::size_t data_start = ehsize + phentsize * segments.size();
    std::size_t size = data_start;
    for (const Segment& s : segments) {
        size += s.bytes.size();
    }
    std::vector<std::uint8_t> out(size, 0);
    const std::uint8_t ident[] = {0x7F, 'E', 'L', 'F', 1, 1, 1};
    std::memcpy(out.data(), ident, sizeof ident);
    put16(out, 16, type);
    put16(out, 18, machine);
    put32(out, 20, 1); // e_version
    put32(out, 24, entry);
    put32(out, 28, static_cast<std::uint32_t>(ehsize)); // e_phoff
    put16(out, 40, static_cast<std::uint16_t>(ehsize));
    put16(out, 42, static_cast<std::uint16_t>(phentsize));
    put16(out, 44, static_cast<std::uint16_t>(segments.size()));
    std::size_t offset = data_start;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const Segment& s = segments[i];
        const std::size_t ph = ehsize + i * phentsize;
        put32(out, ph + 0, 1); // PT_LOAD
        put32(out, ph + 4, static_cast<std::uint32_t>(offset));
        put32(out, ph + 8, s.vaddr);
        put32(out, ph + 12, s.vaddr);
        put32(out, ph + 16, static_cast<std::uint32_t>(s.bytes.size()));
        put32(out, ph + 20, s.memsz);
        put32(out, ph + 24, s.flags);
        put32(out, ph + 28, 16);
        if (!s.bytes.empty()) {
            std::memcpy(out.data() + offset, s.bytes.data(), s.bytes.size());
        }
        offset += s.bytes.size();
    }
    return out;
}

} // namespace test_elf
