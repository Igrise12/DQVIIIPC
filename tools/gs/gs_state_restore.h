// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "gs_stream.h"

#include <cstdint>
#include <vector>

// Restores the GS state a PCSX2 dump starts from through nothing but GIF packets, so a dump replays
// through the same IGsBackend calls the runtime makes (tools/gs/README.md):
// 1. GS memory: one host-to-local IMAGE transfer of a 1024x1024 PSMCT32 rectangle at block 0 whose buffer
//    is 1024 pixels wide, which covers the 4 MiB exactly; the dump's bytes are rearranged into the
//    rectangle's raster order with the GS's PSMCT32 page, block and column layout;
// 2. the general registers through A+D, the transfer registers after the upload. TRXDIR is not written
//    (it would start a transfer) and TEX0's CLUT-load field is cleared (the dump has no CLUT buffer).
// The GIF path state and the CLUT buffer are not restored; dumps taken at a frame boundary have idle paths.
namespace dq8::tools::gs {

// The 32-bit word index in GS memory of pixel (x, y) of a PSMCT32 buffer at block 0, `bw` x 64 pixels wide.
std::uint32_t psmct32_word_index(std::uint32_t x, std::uint32_t y, std::uint32_t bw) noexcept;

// GIF packets (PATH3) that put a backend into `state`.
std::vector<std::uint8_t> state_restore_packets(const DumpState& state);

} // namespace dq8::tools::gs
