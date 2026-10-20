// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "mwo3_builder.h"

#include "iso_builder.h"

#include <algorithm>
#include <stdexcept>

namespace dq8::test {

void put_le32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    if (offset + 4 > bytes.size()) {
        throw std::out_of_range("put_le32: offset beyond the buffer");
    }
    for (std::size_t i = 0; i < 4; ++i) {
        bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

std::vector<std::uint8_t> build_mwo3(const Mwo3Spec& spec) {
    const std::size_t payload = static_cast<std::size_t>(spec.text_size) + spec.data_size;
    std::vector<std::uint8_t> file(64 + payload, 0);
    put_le32(file, 0x00, 0x336F574Du); // "MWo3"
    put_le32(file, 0x04, spec.overlay_id);
    put_le32(file, 0x08, spec.load_address);
    put_le32(file, 0x0C, spec.text_size);
    put_le32(file, 0x10, spec.data_size);
    put_le32(file, 0x14, spec.bss_size);
    put_le32(file, 0x18, spec.table_start());
    put_le32(file, 0x1C, spec.table_end());
    if (spec.name.size() >= 32) {
        throw std::invalid_argument("build_mwo3: name must be shorter than 32 bytes");
    }
    std::copy(spec.name.begin(), spec.name.end(), file.begin() + 0x20);
    const std::vector<std::uint8_t> bytes = seeded_bytes(spec.seed, payload);
    std::copy(bytes.begin(), bytes.end(), file.begin() + 64);
    return file;
}

} // namespace dq8::test
