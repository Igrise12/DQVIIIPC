// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Builds synthetic "MWo3" overlay containers for the unit tests: a 64-byte header from the values below
// over a payload of seeded random bytes. Nothing in a built container comes from a game disc; the sizes
// and addresses are our own. Tests corrupt one word with put_le32() and check the parser's reaction.
namespace dq8::test {

struct Mwo3Spec {
    std::uint32_t overlay_id = 1;
    std::uint32_t load_address = 0x00200000;
    std::uint32_t text_size = 0x200;
    std::uint32_t data_size = 0x80;
    std::uint32_t bss_size = 0x40;
    std::uint32_t table_offset = 0x10; // table_start relative to the start of the data range
    std::uint32_t table_entries = 2;   // table_end = table_start + 4 * table_entries
    std::string name = "synthetic";
    std::uint32_t seed = 1;

    std::uint32_t data_start() const noexcept { return load_address + 64 + text_size; }
    std::uint32_t table_start() const noexcept { return data_start() + table_offset; }
    std::uint32_t table_end() const noexcept { return table_start() + 4 * table_entries; }
};

// The whole container: 64 + text_size + data_size bytes.
std::vector<std::uint8_t> build_mwo3(const Mwo3Spec& spec);

// Overwrites the little-endian u32 at `offset`.
void put_le32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value);

} // namespace dq8::test
