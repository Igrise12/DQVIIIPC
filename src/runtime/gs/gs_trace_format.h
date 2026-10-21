// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/gs/gs_backend.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

// The GS trace format: the packet body of a PCSX2 GS dump with no header, the form parallel-gs's replayer
// reads with --raw (GSDumpParser::open_raw). Each record is a type byte and its body, little-endian:
//   0 transfer        u8 path (PCSX2 GSTransferPath: 0 PATH1, 1 PATH2, 2 PATH3), u32 size, size bytes
//   1 vsync           u8 field (CSR FIELD after the toggle)
//   2 read FIFO       u32 size in quadwords
//   3 priv registers  8,192 bytes (the privileged page)
// A vsync is written as a priv-registers record followed by a vsync record, as PCSX2's dumper does. The GS
// pipeline hashes exactly these bytes (gs_pipeline.h), so a trace file and the stream hash describe the same
// stream.
namespace dq8::gs::trace {

enum class RecordType : std::uint8_t { transfer = 0, vsync = 1, read_fifo = 2, priv_registers = 3 };

inline std::uint8_t path_byte(GifPath path) noexcept {
    switch (path) {
    case GifPath::path1:
        return 0;
    case GifPath::path2:
        return 1;
    case GifPath::path3:
        return 2;
    }
    return 2;
}

inline void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
                               static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 24)};
    out.insert(out.end(), b, b + 4);
}

// The record header of a transfer (the data follows it).
inline void transfer_header(std::vector<std::uint8_t>& out, GifPath path, std::size_t size) {
    out.push_back(static_cast<std::uint8_t>(RecordType::transfer));
    out.push_back(path_byte(path));
    put_u32(out, static_cast<std::uint32_t>(size));
}

inline void vsync_records(std::vector<std::uint8_t>& out, const VsyncInfo& info) {
    out.push_back(static_cast<std::uint8_t>(RecordType::priv_registers));
    out.insert(out.end(), info.priv.page.begin(), info.priv.page.end());
    out.push_back(static_cast<std::uint8_t>(RecordType::vsync));
    out.push_back(info.odd_field ? 1u : 0u);
}

inline void read_fifo_record(std::vector<std::uint8_t>& out, std::size_t bytes) {
    out.push_back(static_cast<std::uint8_t>(RecordType::read_fifo));
    put_u32(out, static_cast<std::uint32_t>(bytes / 16));
}

} // namespace dq8::gs::trace
