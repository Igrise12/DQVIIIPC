// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/gs/priv_regs.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// The GS behind one interface (architecture §6.1, ADR-0004). Everything here runs on the render thread only:
// paraLLEl-GS requires all calls from one thread and blocks it on readbacks. The backend is told GIF
// transfers in arbitration order and, at each vsync, the privileged registers; it reports the scanout.
// Backends: null, trace (records the packet stream in PCSX2's dump packet format, no header), parallel
// (dq8-gs-parallel).
namespace dq8::gs {

enum class GifPath : std::uint8_t { path1 = 1, path2 = 2, path3 = 3 };

struct VsyncInfo {
    std::uint64_t field = 0; // fields since boot
    bool odd_field = false;  // CSR FIELD after the toggle
    PrivRegs priv;           // the privileged-register page as the guest left it
};

struct FrameImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4 bytes, rows top to bottom
    std::uint64_t field = 0;        // the vsync that produced it
};

struct GsCaps {
    std::string_view name; // "null", "trace", "parallel"
    bool produces_pixels = false;
    bool readbacks = false; // local->host transfers return real GS memory
    std::uint32_t max_super_sampling = 1;
};

class IGsBackend {
  public:
    virtual ~IGsBackend() = default;
    virtual GsCaps caps() const = 0;
    virtual void reset() = 0;
    // Whole GIF packets (tags included), a multiple of 16 bytes.
    virtual void gif_transfer(GifPath path, std::span<const std::uint8_t> qwords) = 0;
    virtual void vsync(const VsyncInfo& info) = 0;
    // The latest scanout; false when the backend produces no pixels or has none yet. May block.
    virtual bool read_frame(FrameImage& out) = 0;
    // Data of a local->host transfer (BITBLTBUF/TRXDIR = 1); returns bytes written. May block.
    virtual std::size_t read_transfer(std::span<std::uint8_t> out) = 0;
    virtual void flush() = 0;
};

} // namespace dq8::gs
